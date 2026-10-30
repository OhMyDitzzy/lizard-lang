#include <stddef.h>
#include <stdlib.h>

#include "ge_internal.h"
#include "ge_jit.h"
#include "lizard/array.h"
#include "lizard/native.h"

/*
 * The Gecko Engine executor.
 *
 * Contract: indistinguishable from the reference interpreter. Same results,
 * same output, same error message and source position, same fuel, same
 * vm->instructions. That is kept by three rules:
 *
 *  - Every fast path is guarded by exact type checks and touches nothing until
 *    all of them pass. On any doubt (a string, an overflow, a typed array, a
 *    full stack, an out of range index, ...) it hands the instruction to the
 *    reference interpreter (lz_vm_exec_one), which also produces every error.
 *    GE never builds an error message of its own.
 *  - Fuel and the instruction counter are charged exactly as the reference
 *    does. A fused instruction charges for all the instructions it stands for,
 *    and only when the fuel covers all of them; otherwise it runs plain, so
 *    metering still stops at exactly the same instruction.
 *  - Reference counting is mirrored, not shortcut: a value borrowed instead of
 *    copied is only borrowed when the copy would have been released again
 *    before anything could observe it.
 */

#if defined(__GNUC__) || defined(__clang__)
#define GE_LIKELY(x) __builtin_expect(!!(x), 1)
#define GE_UNLIKELY(x) __builtin_expect(!!(x), 0)
#define GE_INLINE static inline __attribute__((always_inline))
#else
#define GE_LIKELY(x) (x)
#define GE_UNLIKELY(x) (x)
#define GE_INLINE static inline
#endif

/* ---- integer arithmetic that reports overflow (mirrors core/arith.c) ---- */

#if defined(__GNUC__) && !defined(LZ_PORTABLE_OVERFLOW)

GE_INLINE bool add_overflows(int64_t a, int64_t b, int64_t *out) {
    return __builtin_add_overflow(a, b, out);
}

GE_INLINE bool subtract_overflows(int64_t a, int64_t b, int64_t *out) {
    return __builtin_sub_overflow(a, b, out);
}

GE_INLINE bool multiply_overflows(int64_t a, int64_t b, int64_t *out) {
    return __builtin_mul_overflow(a, b, out);
}

#else

GE_INLINE bool add_overflows(int64_t a, int64_t b, int64_t *out) {
    if ((b > 0 && a > INT64_MAX - b) || (b < 0 && a < INT64_MIN - b)) {
        return true;
    }
    *out = a + b;
    return false;
}

GE_INLINE bool subtract_overflows(int64_t a, int64_t b, int64_t *out) {
    if ((b < 0 && a > INT64_MAX + b) || (b > 0 && a < INT64_MIN + b)) {
        return true;
    }
    *out = a - b;
    return false;
}

GE_INLINE bool multiply_overflows(int64_t a, int64_t b, int64_t *out) {
    if (a == 0 || b == 0) {
        *out = 0;
        return false;
    }
    if ((a == -1 && b == INT64_MIN) || (b == -1 && a == INT64_MIN)) {
        return true;
    }
    bool overflow;
    if (a > 0) {
        overflow = b > 0 ? a > INT64_MAX / b : b < INT64_MIN / a;
    } else {
        overflow = b > 0 ? a < INT64_MIN / b : b < INT64_MAX / a;
    }
    if (overflow) {
        return true;
    }
    *out = a * b;
    return false;
}

#endif

/* ---- values ---- */

#define GE_TWO_53 INT64_C(9007199254740992)
#define GE_NIL_INIT {LZ_VAL_NIL, {.integer = 0}}

GE_INLINE LzValue make_bool(bool value) {
    LzValue result = GE_NIL_INIT;
    result.type = LZ_VAL_BOOL;
    result.as.boolean = value;
    return result;
}

GE_INLINE bool is_number(const LzValue *value) {
    return value->type == LZ_VAL_INT || value->type == LZ_VAL_FLOAT;
}

GE_INLINE double as_double(const LzValue *value) {
    return value->type == LZ_VAL_INT ? (double)value->as.integer : value->as.number;
}

/* An int that converts to a double without rounding, so comparing as doubles is exact. */
GE_INLINE bool exact_as_double(const LzValue *value) {
    return value->type == LZ_VAL_FLOAT ||
           (value->type == LZ_VAL_INT && value->as.integer >= -GE_TWO_53 &&
            value->as.integer <= GE_TWO_53);
}

GE_INLINE bool is_truthy(const LzValue *value) {
    if (value->type == LZ_VAL_NIL) {
        return false;
    }
    return value->type != LZ_VAL_BOOL || value->as.boolean;
}

GE_INLINE void grab(const LzValue *value) {
    if (value->type == LZ_VAL_OBJECT) {
        value->as.object->refcount++;
    }
}

GE_INLINE void drop_object(LzVM *vm, LzObject *object) {
    if (GE_LIKELY(object->refcount > 1)) {
        object->refcount--;
    } else {
        lz_object_release(&vm->heap, object); /* the last reference: free it the real way */
    }
}

GE_INLINE void drop(LzVM *vm, const LzValue *value) {
    if (value->type == LZ_VAL_OBJECT) {
        drop_object(vm, value->as.object);
    }
}

/*
 * The result of `a op b` when both are numbers and nothing can go wrong (no
 * overflow, no zero divisor), false otherwise. Mirrors lz_arith_binary for the
 * cases it covers; everything else is left to it.
 */
GE_INLINE bool arith(uint8_t op, const LzValue *a, const LzValue *b, LzValue *out) {
    if (a->type == LZ_VAL_INT && b->type == LZ_VAL_INT) {
        int64_t x = a->as.integer;
        int64_t y = b->as.integer;
        int64_t r = 0;
        switch (op) {
        case LZ_OP_ADD:
            if (add_overflows(x, y, &r)) {
                return false;
            }
            break;
        case LZ_OP_SUBTRACT:
            if (subtract_overflows(x, y, &r)) {
                return false;
            }
            break;
        case LZ_OP_MULTIPLY:
            if (multiply_overflows(x, y, &r)) {
                return false;
            }
            break;
        case LZ_OP_DIVIDE:
            if (y == 0) {
                return false;
            }
            out->type = LZ_VAL_FLOAT;
            out->as.number = (double)x / (double)y;
            return true;
        case LZ_OP_MODULO:
            if (y <= 0) {
                return false; /* zero, -1 and negative divisors are the reference's */
            }
            r = x % y;
            if (r < 0) {
                r += y;
            }
            break;
        case LZ_OP_FLOOR_DIVIDE:
            if (y <= 0) {
                return false;
            }
            r = x / y;
            if (x < 0 && x % y != 0) {
                r--;
            }
            break;
        default:
            return false;
        }
        out->type = LZ_VAL_INT;
        out->as.integer = r;
        return true;
    }

    if (!is_number(a) || !is_number(b)) {
        return false;
    }
    double x = as_double(a);
    double y = as_double(b);
    double r = 0.0;
    switch (op) {
    case LZ_OP_ADD:
        r = x + y;
        break;
    case LZ_OP_SUBTRACT:
        r = x - y;
        break;
    case LZ_OP_MULTIPLY:
        r = x * y;
        break;
    case LZ_OP_DIVIDE:
        if (y == 0.0) {
            return false;
        }
        r = x / y;
        break;
    case LZ_OP_MODULO: {
        /* Whole-number floats (what `n / 2` leaves behind): the remainder is exact in
         * integers and equals what fmod plus the sign fix-up gives, minus the libm call. */
        if (y > 0.0 && y <= (double)GE_TWO_53 && x >= -(double)GE_TWO_53 && x <= (double)GE_TWO_53) {
            int64_t xi = (int64_t)x;
            int64_t yi = (int64_t)y;
            if ((double)xi == x && (double)yi == y) {
                int64_t m = xi % yi;
                if (m < 0) {
                    m += yi;
                }
                r = (double)m;
                break;
            }
        }
        return false;
    }
    default:
        return false; /* float '//' and the like: the reference's */
    }
    out->type = LZ_VAL_FLOAT;
    out->as.number = r;
    return true;
}

/*
 * `a op b` for the five comparisons when both are numbers; false otherwise
 * (strings, arrays, ... and an int too big to compare as a double exactly).
 * NaN makes every comparison false except '==', as in the reference.
 */
GE_INLINE bool compare(uint8_t op, const LzValue *a, const LzValue *b, bool *result) {
    if (a->type == LZ_VAL_INT && b->type == LZ_VAL_INT) {
        int64_t x = a->as.integer;
        int64_t y = b->as.integer;
        switch (op) {
        case LZ_OP_LESS:
            *result = x < y;
            return true;
        case LZ_OP_GREATER:
            *result = x > y;
            return true;
        case LZ_OP_LESS_EQUAL:
            *result = x <= y;
            return true;
        case LZ_OP_GREATER_EQUAL:
            *result = x >= y;
            return true;
        case LZ_OP_EQUAL:
            *result = x == y;
            return true;
        default:
            return false;
        }
    }
    if (is_number(a) && is_number(b) && exact_as_double(a) && exact_as_double(b)) {
        double x = as_double(a);
        double y = as_double(b);
        switch (op) {
        case LZ_OP_LESS:
            *result = x < y;
            return true;
        case LZ_OP_GREATER:
            *result = x > y;
            return true;
        case LZ_OP_LESS_EQUAL:
            *result = x <= y;
            return true;
        case LZ_OP_GREATER_EQUAL:
            *result = x >= y;
            return true;
        case LZ_OP_EQUAL:
            *result = x == y;
            return true;
        default:
            return false;
        }
    }
    return false;
}

/* ---- globals, through an inline cache ---- */

/*
 * The value of a global, or NULL when the name is not a global (it may be a
 * builtin or undefined: the reference decides). *hint remembers the table slot
 * the name was last found in; the key comparison makes a stale hint harmless
 * after the table grows.
 */
GE_INLINE LzValue *global_ref(LzVM *vm, const LzValue *constants, uint16_t name_index,
                              uint32_t *hint) {
    LzTable *table = &vm->globals;
    const LzString *name = (const LzString *)constants[name_index].as.object;
    uint32_t slot = *hint;
    if (GE_LIKELY(slot < table->capacity && table->entries[slot].key == name)) {
        return &table->entries[slot].value;
    }
    const LzValue *found = lz_table_get(table, name);
    if (found == NULL) {
        return NULL;
    }
    const LzTableEntry *entry =
        (const LzTableEntry *)((const char *)found - offsetof(LzTableEntry, value));
    *hint = (uint32_t)(entry - table->entries);
    return &table->entries[*hint].value;
}


/*
 * Builtins, through a per-site cache. Globals shadow builtins, so a cached builtin
 * is only valid while no global has been added since it was cached: in->target holds
 * the size of the globals table at that moment. This relies on tables never removing
 * a key (true today, see core/table.h); a table that learns to delete needs an epoch
 * counter here instead.
 */
GE_INLINE const LzValue *builtin_cached(LzVM *vm, const LzValue *constants, const GeIns *in) {
    const LzTable *builtins = &vm->builtins;
    uint32_t slot = in->hint_b;
    if (GE_LIKELY(in->target == (uint32_t)vm->globals.count && slot < builtins->capacity &&
                  builtins->entries[slot].key == (const LzString *)constants[in->a].as.object)) {
        return &builtins->entries[slot].value;
    }
    return NULL;
}

/* Looks the name up as a builtin and remembers where. Only for a name that is not a global. */
GE_INLINE const LzValue *builtin_fill(LzVM *vm, const LzValue *constants, GeIns *in) {
    const LzString *name = (const LzString *)constants[in->a].as.object;
    const LzValue *found = lz_table_get(&vm->builtins, name);
    if (found == NULL) {
        return NULL; /* undefined: the reference reports it */
    }
    const LzTableEntry *entry =
        (const LzTableEntry *)((const char *)found - offsetof(LzTableEntry, value));
    in->hint_b = (uint32_t)(entry - vm->builtins.entries);
    in->target = (uint32_t)vm->globals.count;
    return found;
}

/*
 * Where an assignment to an existing global goes. NULL when the reference has to
 * do it: the global does not exist, or the table is about to grow (lz_table_set
 * grows before it looks for the key, even when the key exists).
 */
GE_INLINE LzValue *global_store(LzVM *vm, const LzValue *constants, uint16_t name_index,
                                uint32_t *hint) {
    const LzTable *table = &vm->globals;
    if (GE_UNLIKELY(table->count + 1 > table->capacity * 3 / 4)) {
        return NULL;
    }
    return global_ref(vm, constants, name_index, hint);
}

/* A fused operand: a local, a constant or a global. NULL means "run it plain". */
GE_INLINE const LzValue *load_operand(LzVM *vm, const LzValue *constants, const LzValue *stack,
                                      size_t base, size_t sp, unsigned kind, uint16_t index,
                                      uint32_t *hint) {
    if (kind == GE_K_LOCAL) {
        return base + index < sp ? &stack[base + index] : NULL;
    }
    if (kind == GE_K_CONST) {
        return &constants[index];
    }
    return global_ref(vm, constants, index, hint);
}

/* array[index] for an int index in range (negative counts from the end). */
GE_INLINE bool array_position(const LzValue *container, const LzValue *index, LzArray **array,
                              size_t *position) {
    if (!lz_value_is_array(container) || index->type != LZ_VAL_INT) {
        return false;
    }
    LzArray *target = lz_value_as_array(container);
    int64_t wanted = index->as.integer;
    int64_t resolved = wanted < 0 ? wanted + (int64_t)target->count : wanted;
    if (resolved < 0 || resolved >= (int64_t)target->count) {
        return false;
    }
    *array = target;
    *position = (size_t)resolved;
    return true;
}

/*
 * The translated program of a function, made on its first call and kept on the function
 * object (so it lives exactly as long as the function does). NULL when the function's
 * code cannot be translated; the caller then hands that call to the reference interpreter.
 */
static GeProgram *function_program(LzFunction *function, GeRun *run) {
    if (GE_LIKELY(function->engine_cache != NULL)) {
        return function->engine_cache;
    }
    GeProgram *program = malloc(sizeof *program);
    if (program == NULL) {
        return NULL;
    }
    if (!ge_translate(&function->chunk, program)) {
        free(program);
        return NULL;
    }
    function->engine_cache = program;
    function->engine_cache_free = ge_cache_free;
    run->function_ops += program->count;
    run->function_fused += program->fused_sites;
    return program;
}

#define KIND_A(in) ((unsigned)((in)->kinds & 3u))
#define KIND_B(in) ((unsigned)(((in)->kinds >> 2) & 3u))
#define KIND_S(in) ((unsigned)(((in)->kinds >> 4) & 3u))

LzStatus ge_execute(LzVM *vm, const LzChunk *main_chunk, size_t *failed_at, void *context) {
    GeRun *run = context;
    /* The code being run changes with every call and return; these always describe the top frame. */
    const LzChunk *chunk = main_chunk;
    GeIns *code = run->program->ins;
    const LzValue *constants = chunk->constants;
    size_t base = 0; /* stack index of the frame's local slot 0 */
    size_t forced_fail = SIZE_MAX; /* failure offset reported by a call run on the reference interpreter */
    LzValue *const stack = vm->stack;
    size_t sp = vm->stack_count;
    size_t pc = 0;
    uint64_t executed = 0;
    uint64_t dispatches = 0;
    uint64_t delegated = 0;
    LzStatus status = LZ_OK;
    GeIns *ins = code;
    GeProgram *prog = run->program;
    GeJitExit jx;
    uint32_t nt_count = 0; /* hotness to add at the next native_try */

/* The stack height and instruction count must be visible to anything that calls out. */
#define SYNC_OUT()                                                                                 \
    do {                                                                                           \
        vm->stack_count = sp;                                                                      \
        vm->instructions += executed;                                                              \
        executed = 0;                                                                              \
    } while (0)
#define SYNC_IN() (sp = vm->stack_count)

/* Charge fuel for n instructions and count them. Fails exactly where the reference would. */
#define TICK(n)                                                                                    \
    do {                                                                                           \
        if (GE_UNLIKELY(vm->fuel != LZ_FUEL_UNLIMITED)) {                                          \
            status = lz_vm_charge_fuel(vm, (n));                                                   \
            if (status != LZ_OK) {                                                                 \
                goto fail;                                                                         \
            }                                                                                      \
        }                                                                                          \
        executed += (n);                                                                           \
    } while (0)

/*
 * A fused instruction may only run when the fuel covers every instruction it stands
 * for and the stack has room for the deepest point the plain sequence reaches. It
 * charges (FUSED_COMMIT) only after every check has passed, so falling back leaves
 * no trace.
 */
#define FUSED_GUARD(peak)                                                                          \
    do {                                                                                           \
        if (GE_UNLIKELY(vm->fuel != LZ_FUEL_UNLIMITED && vm->fuel < ins->span)) {                  \
            goto plain_tick;                                                                       \
        }                                                                                          \
        if (GE_UNLIKELY(sp + (peak) > LZ_STACK_MAX)) {                                             \
            goto plain_tick;                                                                       \
        }                                                                                          \
    } while (0)
#define FUSED_COMMIT()                                                                             \
    do {                                                                                           \
        if (GE_UNLIKELY(vm->fuel != LZ_FUEL_UNLIMITED)) {                                          \
            vm->fuel -= ins->span;                                                                 \
        }                                                                                          \
        executed += ins->span;                                                                     \
    } while (0)

    if (run->jit && run->jit_eager) {
        goto native_try;
    }

    for (;;) {
        ins = &code[pc];
        dispatches++;
        switch (ins->op) {
        case GE_OP_END:
            /* Ran off the end of the chunk. The reference reports it before charging fuel. */
            goto run_plain;

        case LZ_OP_CONSTANT: {
            TICK(1);
            if (GE_UNLIKELY(sp >= LZ_STACK_MAX)) {
                goto run_plain;
            }
            LzValue value = constants[ins->a];
            grab(&value);
            stack[sp++] = value;
            pc++;
            break;
        }
        case LZ_OP_NIL:
        case LZ_OP_TRUE:
        case LZ_OP_FALSE: {
            TICK(1);
            if (GE_UNLIKELY(sp >= LZ_STACK_MAX)) {
                goto run_plain;
            }
            LzValue value = GE_NIL_INIT;
            if (ins->op != LZ_OP_NIL) {
                value = make_bool(ins->op == LZ_OP_TRUE);
            }
            stack[sp++] = value;
            pc++;
            break;
        }
        case LZ_OP_POP:
            TICK(1);
            if (GE_UNLIKELY(sp == 0)) {
                goto run_plain;
            }
            sp--;
            drop(vm, &stack[sp]);
            pc++;
            break;

        case LZ_OP_GET_LOCAL: {
            TICK(1);
            if (GE_UNLIKELY(base + ins->a >= sp || sp >= LZ_STACK_MAX)) {
                goto run_plain;
            }
            LzValue value = stack[base + ins->a];
            grab(&value);
            stack[sp++] = value;
            pc++;
            break;
        }
        case LZ_OP_SET_LOCAL: {
            TICK(1);
            if (GE_UNLIKELY(sp == 0 || base + ins->a >= sp - 1)) {
                goto run_plain;
            }
            LzValue moved = stack[--sp];
            LzValue old = stack[base + ins->a];
            stack[base + ins->a] = moved;
            drop(vm, &old);
            pc++;
            break;
        }
        case LZ_OP_GET_GLOBAL: {
            TICK(1);
            const LzValue *found = builtin_cached(vm, constants, ins);
            if (found == NULL) {
                found = global_ref(vm, constants, ins->a, &ins->hint_a);
                if (found == NULL) {
                    found = builtin_fill(vm, constants, ins); /* not a global: maybe a builtin */
                }
            }
            if (GE_UNLIKELY(found == NULL || sp >= LZ_STACK_MAX)) {
                goto run_plain; /* undefined: the reference reports it */
            }
            LzValue value = *found;
            grab(&value);
            stack[sp++] = value;
            pc++;
            break;
        }
        case LZ_OP_SET_GLOBAL: {
            TICK(1);
            LzValue *slot = sp > 0 ? global_store(vm, constants, ins->a, &ins->hint_a) : NULL;
            if (GE_UNLIKELY(slot == NULL)) {
                goto run_plain;
            }
            /* The table takes over the stack's reference: the reference's retain + pop. */
            LzValue old = *slot;
            *slot = stack[--sp];
            drop(vm, &old);
            pc++;
            break;
        }

#define ARITH_CASE(OPC)                                                                            \
    case OPC: {                                                                                    \
        TICK(1);                                                                                   \
        LzValue out = GE_NIL_INIT;                                                                 \
        if (GE_UNLIKELY(sp < 2 || !arith(OPC, &stack[sp - 2], &stack[sp - 1], &out))) {            \
            goto run_plain;                                                                        \
        }                                                                                          \
        stack[sp - 2] = out;                                                                       \
        sp--;                                                                                      \
        pc++;                                                                                      \
        break;                                                                                     \
    }
            ARITH_CASE(LZ_OP_ADD)
            ARITH_CASE(LZ_OP_SUBTRACT)
            ARITH_CASE(LZ_OP_MULTIPLY)
            ARITH_CASE(LZ_OP_DIVIDE)
            ARITH_CASE(LZ_OP_FLOOR_DIVIDE)
            ARITH_CASE(LZ_OP_MODULO)

#define COMPARE_CASE(OPC)                                                                          \
    case OPC: {                                                                                    \
        TICK(1);                                                                                   \
        bool outcome = false;                                                                      \
        if (GE_UNLIKELY(sp < 2 || !compare(OPC, &stack[sp - 2], &stack[sp - 1], &outcome))) {      \
            goto run_plain;                                                                        \
        }                                                                                          \
        stack[sp - 2] = make_bool(outcome);                                                        \
        sp--;                                                                                      \
        pc++;                                                                                      \
        break;                                                                                     \
    }
            COMPARE_CASE(LZ_OP_LESS)
            COMPARE_CASE(LZ_OP_GREATER)
            COMPARE_CASE(LZ_OP_LESS_EQUAL)
            COMPARE_CASE(LZ_OP_GREATER_EQUAL)
            COMPARE_CASE(LZ_OP_EQUAL)

        case LZ_OP_NOT:
            TICK(1);
            if (GE_UNLIKELY(sp < 1 || stack[sp - 1].type == LZ_VAL_OBJECT)) {
                goto run_plain;
            }
            stack[sp - 1] = make_bool(!is_truthy(&stack[sp - 1]));
            pc++;
            break;

        case LZ_OP_JUMP:
            TICK(1);
            pc = ins->target;
            break;
        case LZ_OP_LOOP:
            TICK(1);
            pc = ins->target;
            nt_count = 1;
            goto native_try; /* a back-edge: hot code gets compiled and entered here */
        case LZ_OP_JUMP_IF_FALSE:
        case LZ_OP_JUMP_IF_TRUE:
            TICK(1);
            if (GE_UNLIKELY(sp == 0)) {
                goto run_plain;
            }
            pc = is_truthy(&stack[sp - 1]) == (ins->op == LZ_OP_JUMP_IF_TRUE) ? ins->target : pc + 1;
            break;

        case LZ_OP_INDEX: {
            TICK(1);
            LzArray *array = NULL;
            size_t position = 0;
            if (GE_UNLIKELY(sp < 2 || !array_position(&stack[sp - 2], &stack[sp - 1], &array, &position))) {
                goto run_plain;
            }
            LzValue item = array->items[position];
            grab(&item); /* keep it alive across the release, which may free the array */
            LzObject *holder = stack[sp - 2].as.object;
            stack[sp - 2] = item;
            sp--; /* the index is an int: nothing to release */
            drop_object(vm, holder);
            pc++;
            break;
        }
        case LZ_OP_SET_INDEX: {
            TICK(1);
            LzArray *array = NULL;
            size_t position = 0;
            if (GE_UNLIKELY(sp < 3 || !array_position(&stack[sp - 3], &stack[sp - 2], &array, &position))) {
                goto run_plain;
            }
            const LzValue *value = &stack[sp - 1];
            /* Only the common case: scalar over scalar. Arrays inside arrays need holder
             * bookkeeping and a cycle check, strings need refcounts: the reference's. */
            if (GE_UNLIKELY(value->type == LZ_VAL_OBJECT || array->items[position].type == LZ_VAL_OBJECT ||
                            (array->element != NULL && !lz_type_matches(array->element, value)))) {
                goto run_plain;
            }
            array->items[position] = *value;
            LzObject *holder = stack[sp - 3].as.object;
            sp -= 3;
            drop_object(vm, holder);
            pc++;
            break;
        }

        case LZ_OP_NEGATE: {
            TICK(1);
            LzValue *top = sp > 0 ? &stack[sp - 1] : NULL;
            if (GE_UNLIKELY(top == NULL)) {
                goto run_plain;
            }
            if (top->type == LZ_VAL_INT && top->as.integer != INT64_MIN) {
                top->as.integer = -top->as.integer;
            } else if (top->type == LZ_VAL_FLOAT) {
                top->as.number = -top->as.number;
            } else {
                goto run_plain; /* overflow, or not a number: the reference's error */
            }
            pc++;
            break;
        }
        case LZ_OP_CALL: {
            TICK(1);
            size_t argc = ins->a;
            if (GE_UNLIKELY(sp < argc + 1 || stack[sp - argc - 1].type != LZ_VAL_OBJECT)) {
                goto run_plain; /* "can only call functions" */
            }
            LzObject *callee_object = stack[sp - argc - 1].as.object;

            if (callee_object->type == LZ_OBJ_CLOSURE) {
                LzClosure *closure = (LzClosure *)callee_object;
                LzFunction *function = closure->function;
                GeProgram *callee_program = function->engine_cache;
                if (GE_LIKELY(argc == function->param_count && !function->has_rest &&
                              callee_program != NULL && vm->frame_count < LZ_FRAMES_MAX)) {
                    /* Exactly the arguments the function wants: the frame starts right here. */
                    vm->frames[vm->frame_count - 1].ip = pc + 1;
                    LzCallFrame *frame = &vm->frames[vm->frame_count++];
                    frame->closure = closure;
                    frame->chunk = &function->chunk;
                    frame->ip = 0;
                    frame->base = sp - argc;
                    frame->supplied = LZ_ALL_SUPPLIED;
                    frame->call_offset = ins->offset;
                    base = sp - argc;
                    chunk = &function->chunk;
                    code = callee_program->ins;
                    prog = callee_program;
                    constants = chunk->constants;
                    pc = 0;
                    nt_count = 1;
                    goto native_try;
                }
                /* Defaults, a rest parameter, an arity error or a function not translated yet. */
                bool entered = false;
                SYNC_OUT();
                status = lz_vm_call(vm, argc, pc + 1, ins->offset, &entered);
                SYNC_IN();
                if (status != LZ_OK) {
                    goto fail;
                }
                goto enter_callee;
            }
            if (GE_UNLIKELY(callee_object->type != LZ_OBJ_NATIVE)) {
                goto run_plain;
            }
            /* What the reference's call does: the native sees the stack as it is, the result
             * replaces the callee and its arguments. On failure nothing is popped. */
            LzValue result = GE_NIL_INIT;
            SYNC_OUT();
            vm->native_call_offset = ins->offset;
            status = lz_native_call(vm, (const LzNative *)callee_object, (int)argc, &stack[sp - argc], &result);
            if (status != LZ_OK) {
                goto fail;
            }
            for (size_t i = 0; i < argc + 1; i++) {
                sp--;
                drop(vm, &stack[sp]);
            }
            stack[sp++] = result;
            pc++;
            break;
        }

        case LZ_OP_CALL_EX: {
            TICK(1);
            bool entered = false;
            SYNC_OUT();
            status = lz_vm_call_ex(vm, chunk, ins->a, ins->b, pc + 1, ins->offset, &entered);
            SYNC_IN();
            if (status != LZ_OK) {
                goto fail;
            }
            if (entered) {
                goto enter_callee;
            }
            pc++;
            break;
        }

        case LZ_OP_JUMP_IF_SUPPLIED:
            TICK(1);
            pc = (ins->a >= 64 || ((vm->frames[vm->frame_count - 1].supplied >> ins->a) & 1u) != 0)
                     ? ins->target
                     : pc + 1;
            break;

        case LZ_OP_RETURN: {
            TICK(1);
            if (vm->frame_count <= 1) {
                status = LZ_OK;
                goto done;
            }
            const LzCallFrame *frame = &vm->frames[vm->frame_count - 1];
            if (GE_LIKELY(sp > base && base > 0 &&
                          (vm->open_upvalues == NULL || vm->open_upvalues->location < &stack[base]))) {
                /* No variable of this frame was captured: drop the frame and keep the result. */
                LzValue result = stack[--sp];
                while (sp > base - 1) {
                    sp--;
                    drop(vm, &stack[sp]);
                }
                stack[sp++] = result;
                vm->frame_count--;
            } else {
                (void)frame;
                SYNC_OUT();
                status = lz_vm_return(vm);
                SYNC_IN();
                if (status != LZ_OK) {
                    goto fail;
                }
            }
            const LzCallFrame *caller = &vm->frames[vm->frame_count - 1];
            pc = caller->ip;
            base = caller->base;
            if (caller->closure == NULL) {
                chunk = main_chunk;
                code = run->program->ins;
            } else {
                chunk = caller->chunk;
                prog = caller->closure->function->engine_cache;
                code = prog->ins;
            }
            if (caller->closure == NULL) {
                prog = run->program;
            }
            constants = chunk->constants;
            nt_count = 0;
            goto native_try;
        }

        /* ---- fused instructions ---- */

        case GE_OP_BIN: { /* [load a] [load b] ARITH [store] */
            FUSED_GUARD(2);
            const LzValue *a = load_operand(vm, constants, stack, base, sp, KIND_A(ins), ins->a, &ins->hint_a);
            const LzValue *b = load_operand(vm, constants, stack, base, sp, KIND_B(ins), ins->b, &ins->hint_b);
            LzValue out = GE_NIL_INIT;
            if (GE_UNLIKELY(a == NULL || b == NULL || !arith(ins->sub, a, b, &out))) {
                goto plain_tick;
            }
            switch (KIND_S(ins)) {
            case GE_ST_NONE:
                stack[sp++] = out;
                break;
            case GE_ST_LOCAL: {
                if (GE_UNLIKELY(base + ins->c >= sp)) {
                    goto plain_tick;
                }
                LzValue old = stack[base + ins->c];
                stack[base + ins->c] = out;
                drop(vm, &old);
                break;
            }
            default: {
                LzValue *slot = global_store(vm, constants, ins->c, &ins->hint_c);
                if (GE_UNLIKELY(slot == NULL)) {
                    goto plain_tick;
                }
                LzValue old = *slot;
                *slot = out;
                drop(vm, &old);
                break;
            }
            }
            FUSED_COMMIT();
            pc += ins->span;
            break;
        }
        case GE_OP_BIN1: { /* [load b] ARITH [store], left operand on the stack */
            FUSED_GUARD(1);
            if (GE_UNLIKELY(sp < 1)) {
                goto plain_tick;
            }
            const LzValue *a = &stack[sp - 1];
            const LzValue *b = load_operand(vm, constants, stack, base, sp, KIND_B(ins), ins->b, &ins->hint_b);
            LzValue out = GE_NIL_INIT;
            if (GE_UNLIKELY(b == NULL || !arith(ins->sub, a, b, &out))) {
                goto plain_tick;
            }
            switch (KIND_S(ins)) {
            case GE_ST_NONE:
                stack[sp - 1] = out; /* the left operand is a number: nothing to release */
                break;
            case GE_ST_LOCAL: {
                if (GE_UNLIKELY(base + ins->c >= sp - 1)) {
                    goto plain_tick;
                }
                LzValue old = stack[base + ins->c];
                stack[base + ins->c] = out;
                sp--;
                drop(vm, &old);
                break;
            }
            default: {
                LzValue *slot = global_store(vm, constants, ins->c, &ins->hint_c);
                if (GE_UNLIKELY(slot == NULL)) {
                    goto plain_tick;
                }
                LzValue old = *slot;
                *slot = out;
                sp--;
                drop(vm, &old);
                break;
            }
            }
            FUSED_COMMIT();
            pc += ins->span;
            break;
        }
        case GE_OP_CMPJ: { /* [load a] [load b] CMP JUMP_IF_FALSE POP */
            FUSED_GUARD(2);
            const LzValue *a = load_operand(vm, constants, stack, base, sp, KIND_A(ins), ins->a, &ins->hint_a);
            const LzValue *b = load_operand(vm, constants, stack, base, sp, KIND_B(ins), ins->b, &ins->hint_b);
            bool outcome = false;
            if (GE_UNLIKELY(a == NULL || b == NULL || !compare(ins->sub, a, b, &outcome))) {
                goto plain_tick;
            }
            FUSED_COMMIT();
            pc = outcome ? pc + ins->span : ins->target;
            break;
        }
        case GE_OP_CMPJ1: { /* [load b] CMP JUMP_IF_FALSE POP, left operand on the stack */
            FUSED_GUARD(1);
            if (GE_UNLIKELY(sp < 1)) {
                goto plain_tick;
            }
            const LzValue *b = load_operand(vm, constants, stack, base, sp, KIND_B(ins), ins->b, &ins->hint_b);
            bool outcome = false;
            if (GE_UNLIKELY(b == NULL || !compare(ins->sub, &stack[sp - 1], b, &outcome))) {
                goto plain_tick;
            }
            sp--; /* the left operand, a number */
            FUSED_COMMIT();
            pc = outcome ? pc + ins->span : ins->target;
            break;
        }
        case GE_OP_INDEX: { /* [load a] [load b] INDEX: the array is borrowed, not copied */
            FUSED_GUARD(2);
            const LzValue *a = load_operand(vm, constants, stack, base, sp, KIND_A(ins), ins->a, &ins->hint_a);
            const LzValue *b = load_operand(vm, constants, stack, base, sp, KIND_B(ins), ins->b, &ins->hint_b);
            LzArray *array = NULL;
            size_t position = 0;
            if (GE_UNLIKELY(a == NULL || b == NULL || !array_position(a, b, &array, &position))) {
                goto plain_tick;
            }
            LzValue item = array->items[position];
            grab(&item);
            stack[sp++] = item;
            FUSED_COMMIT();
            pc += ins->span;
            break;
        }
        case GE_OP_INDEX1: { /* [load b] INDEX, array on the stack */
            FUSED_GUARD(1);
            if (GE_UNLIKELY(sp < 1)) {
                goto plain_tick;
            }
            const LzValue *b = load_operand(vm, constants, stack, base, sp, KIND_B(ins), ins->b, &ins->hint_b);
            LzArray *array = NULL;
            size_t position = 0;
            if (GE_UNLIKELY(b == NULL || !array_position(&stack[sp - 1], b, &array, &position))) {
                goto plain_tick;
            }
            LzValue item = array->items[position];
            grab(&item);
            LzObject *holder = stack[sp - 1].as.object;
            stack[sp - 1] = item;
            drop_object(vm, holder);
            FUSED_COMMIT();
            pc += ins->span;
            break;
        }

        case GE_OP_DELEGATE:
        default:
            goto plain_tick;
        }
        continue;

    /* A frame was just pushed for a call that needed the slow path: start running it. */
    enter_callee : {
        const LzCallFrame *frame = &vm->frames[vm->frame_count - 1];
        LzFunction *function = frame->closure->function;
        GeProgram *program = function_program(function, run);
        if (program == NULL) {
            /* Code GE cannot translate: the reference interpreter runs this call. */
            size_t depth = vm->frame_count - 1;
            size_t nested = 0;
            SYNC_OUT();
            status = lz_vm_run_until(vm, depth, &nested);
            SYNC_IN();
            delegated++;
            if (status != LZ_OK) {
                forced_fail = nested;
                goto fail;
            }
            pc = vm->frames[vm->frame_count - 1].ip;
            continue;
        }
        base = frame->base;
        chunk = &function->chunk;
        code = program->ins;
        prog = program;
        constants = chunk->constants;
        pc = 0;
        nt_count = 1;
        goto native_try;
    }

    /*
     * Hot code runs as machine code (ge_jit.c). It returns here when it finishes, hands an
     * instruction back, or fails; the stack and frames are always in the interpreter's form.
     */
    native_try : {
        if (!run->jit || vm->fuel != LZ_FUEL_UNLIMITED || prog->jit_state >= GE_JIT_FAILED) {
            continue;
        }
        if (prog->jit == NULL) {
            if (prog->jit_state != GE_JIT_UNTRIED) {
                continue;
            }
            prog->hot += nt_count;
            if (!run->jit_eager && prog->hot < run->jit_threshold) {
                continue;
            }
            const LzCallFrame *top = &vm->frames[vm->frame_count - 1];
            size_t entry_depth = top->closure == NULL ? 0 : lz_function_slots(top->closure->function);
            prog->jit = ge_jit_compile(chunk, prog, entry_depth);
            prog->jit_state = prog->jit != NULL ? GE_JIT_COMPILED : GE_JIT_FAILED;
            if (prog->jit == NULL) {
                continue;
            }
            run->jit_programs++;
            run->jit_bytes += prog->jit->code_size;
        }
        SYNC_OUT();
        if (!ge_jit_enter(vm, prog->jit, pc, base, sp, &jx)) {
            continue;
        }
        SYNC_IN();
        run->jit_enters++;
        prog->jit_enters++;
        dispatches++;
        if (jx.reason == GE_JX_HALT) {
            status = LZ_OK;
            goto done;
        }
        if (jx.reason == GE_JX_ENTER) {
            goto enter_callee;
        }
        {
            const LzCallFrame *top = &vm->frames[vm->frame_count - 1];
            base = top->base;
            if (top->closure == NULL) {
                chunk = main_chunk;
                prog = run->program;
            } else {
                chunk = top->chunk;
                prog = top->closure->function->engine_cache;
            }
            code = prog->ins;
            constants = chunk->constants;
            if (jx.reason == GE_JX_RETURNED) {
                pc = top->ip;
                nt_count = 0;
                goto native_try;
            }
        }
        pc = jx.pc;
        if (jx.reason == GE_JX_FAIL) {
            status = (LzStatus)jx.status;
            goto fail;
        }
        /* BAIL: the interpreter runs this instruction. Give up on code that keeps bailing. */
        prog->jit_bails++;
        if (!run->jit_eager && prog->jit_bails > 256 && prog->jit_bails * 2 > prog->jit_enters) {
            prog->jit_state = GE_JIT_DISABLED;
            prog->jit_retired = prog->jit;
            prog->jit = NULL;
        }
        continue;
    }

    /* Run the plain instruction at this index on the reference interpreter. */
    plain_tick:
        TICK(1);
    run_plain : {
        size_t offset = ins->offset;
        bool halted = false;
        SYNC_OUT();
        status = lz_vm_exec_one(vm, chunk, &offset, &halted);
        SYNC_IN();
        delegated++;
        if (status != LZ_OK) {
            goto fail;
        }
        if (halted) {
            goto done;
        }
        pc++;
        nt_count = 0;
        goto native_try;
    }
    }

fail:
    *failed_at = forced_fail != SIZE_MAX ? forced_fail : code[pc].offset;
done:
    vm->stack_count = sp;
    vm->instructions += executed;
    run->dispatches = dispatches;
    run->delegated = delegated;
    return status;
}
