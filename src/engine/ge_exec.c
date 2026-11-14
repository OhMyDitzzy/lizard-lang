#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "ge_internal.h"
#include "ge_jit.h"
#include "lizard/arith.h"
#include "lizard/array.h"
#include "lizard/class.h"
#include "lizard/dict.h"
#include "lizard/module.h"
#include "lizard/native.h"

/*
 * The Gecko Engine executor.
 *
 * The contract is that it's indistinguishable from the reference interpreter, with the same
 * results, output, error message and source position, fuel and vm->instructions. Three rules keep
 * that.
 *
 * Every fast path is guarded by exact type checks and touches nothing until all of them pass. On
 * any doubt, like a string, an overflow, a typed array, a full stack or an out of range index, it
 * hands the instruction to the reference interpreter (lz_vm_exec_one), which also produces every
 * error. GE never builds an error message of its own.
 *
 * Fuel and the instruction counter are charged exactly as the reference does. A fused instruction
 * charges for all the instructions it stands for, and only when the fuel covers all of them.
 * Otherwise it runs plain, so metering still stops at exactly the same instruction.
 *
 * Reference counting is mirrored and not shortcut. A value borrowed instead of copied is only
 * borrowed when the copy would have been released again before anything could observe it.
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
        lz_object_release(&vm->heap, object); /* the last reference, so free it the real way */
    }
}

GE_INLINE void drop(LzVM *vm, const LzValue *value) {
    if (value->type == LZ_VAL_OBJECT) {
        drop_object(vm, value->as.object);
    }
}

/*
 * The result of `a op b` when both are numbers and nothing can go wrong, meaning no overflow and
 * no zero divisor, and false otherwise. Mirrors lz_arith_binary for the cases it covers and
 * leaves everything else to it.
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
        /*
         * Whole-number floats, which is what `n / 2` leaves behind. The remainder is exact in
         * integers and equals what fmod plus the sign fix-up gives, minus the libm call.
         */
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
        return false; /* float '//' and the like are the reference's */
    }
    out->type = LZ_VAL_FLOAT;
    out->as.number = r;
    return true;
}

/*
 * `a op b` for the bitwise operators when both are ints and nothing can go wrong, and false
 * otherwise. A shift count outside 0..63 is the reference's error. Mirrors lz_arith_binary.
 */
GE_INLINE bool bitwise(uint8_t op, const LzValue *a, const LzValue *b, LzValue *out) {
    if (a->type != LZ_VAL_INT || b->type != LZ_VAL_INT) {
        return false; /* the reference reports the type error */
    }
    int64_t x = a->as.integer;
    int64_t y = b->as.integer;
    int64_t r;
    switch (op) {
    case LZ_OP_BIT_AND:
        r = x & y;
        break;
    case LZ_OP_BIT_OR:
        r = x | y;
        break;
    case LZ_OP_BIT_XOR:
        r = x ^ y;
        break;
    case LZ_OP_SHL:
        if (!lz_shift_count_ok(y)) {
            return false;
        }
        r = lz_shift_left(x, y);
        break;
    case LZ_OP_SHR:
        if (!lz_shift_count_ok(y)) {
            return false;
        }
        r = lz_shift_right(x, y);
        break;
    default:
        return false;
    }
    out->type = LZ_VAL_INT;
    out->as.integer = r;
    return true;
}

/*
 * `a op b` for the five comparisons when both are numbers, and false otherwise, as for strings,
 * arrays and an int too big to compare as a double exactly. NaN makes every comparison false
 * except '==', as in the reference.
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

/*
 * The value of a global, or NULL when the name is not a global. It may be a builtin or
 * undefined, and the reference decides. *hint remembers the table slot the name was last found
 * in, and the key comparison makes a stale hint harmless after the table grows.
 */
GE_INLINE LzValue *global_ref(LzTable *table, const LzValue *constants, uint16_t name_index,
                              uint32_t *hint) {
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
 * Builtins go through a per-site cache. Globals shadow builtins, so a cached builtin is only
 * valid while no global has been added since it was cached, and in->target holds the size of the
 * globals table at that moment. This relies on tables never removing a key, which is true today,
 * see core/table.h. A table that learns to delete needs an epoch counter here instead.
 */
GE_INLINE const LzValue *builtin_cached(LzVM *vm, const LzTable *globals, const LzValue *constants,
                                        const GeIns *in) {
    const LzTable *builtins = &vm->builtins;
    uint32_t slot = in->hint_b;
    if (GE_LIKELY(in->target == (uint32_t)globals->count && slot < builtins->capacity &&
                  builtins->entries[slot].key == (const LzString *)constants[in->a].as.object)) {
        return &builtins->entries[slot].value;
    }
    return NULL;
}

/* Looks the name up as a builtin and remembers where. Only for a name that is not a global. */
GE_INLINE const LzValue *builtin_fill(LzVM *vm, const LzTable *globals, const LzValue *constants,
                                      GeIns *in) {
    const LzString *name = (const LzString *)constants[in->a].as.object;
    const LzValue *found = lz_table_get(&vm->builtins, name);
    if (found == NULL) {
        return NULL; /* undefined, so the reference reports it */
    }
    const LzTableEntry *entry =
        (const LzTableEntry *)((const char *)found - offsetof(LzTableEntry, value));
    in->hint_b = (uint32_t)(entry - vm->builtins.entries);
    in->target = (uint32_t)globals->count;
    return found;
}

/*
 * Where an assignment to an existing global goes. NULL when the reference has to do it, because
 * the global doesn't exist or the table is about to grow. lz_table_set grows before it looks for
 * the key, even when the key exists.
 */
GE_INLINE LzValue *global_store(LzTable *table, const LzValue *constants, uint16_t name_index,
                                uint32_t *hint) {
    if (GE_UNLIKELY(table->count + 1 > table->capacity * 3 / 4)) {
        return NULL;
    }
    return global_ref(table, constants, name_index, hint);
}

/* A fused operand, which is a local, a constant or a global. NULL means "run it plain". */
GE_INLINE const LzValue *load_operand(LzTable *globals, const LzValue *constants, const LzValue *stack,
                                      size_t base, size_t sp, unsigned kind, uint16_t index,
                                      uint32_t *hint) {
    if (kind == GE_K_LOCAL) {
        return base + index < sp ? &stack[base + index] : NULL;
    }
    if (kind == GE_K_CONST) {
        return &constants[index];
    }
    return global_ref(globals, constants, index, hint);
}

/* array[index] for an int index in range, where a negative one counts from the end. */
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
 * Does a nil, bool, int, float or string fit a type that lists its kind? That is all
 * lz_type_matches would conclude for these values, and what the reference's check_value accepts
 * without more ado. Anything else (an object, a type of literals only, a miss) is the
 * reference's, which also writes every message.
 */
GE_INLINE bool lz_value_is_text(const LzValue *value) {
    return value->type == LZ_VAL_OBJECT && value->as.object->type == LZ_OBJ_STRING;
}

GE_INLINE bool kind_accepted(const LzType *type, const LzValue *value) {
    uint16_t kind;
    switch (value->type) {
    case LZ_VAL_NIL:
        kind = LZ_KIND_NIL;
        break;
    case LZ_VAL_BOOL:
        kind = LZ_KIND_BOOL;
        break;
    case LZ_VAL_INT:
        kind = LZ_KIND_INT;
        break;
    case LZ_VAL_FLOAT:
        kind = LZ_KIND_FLOAT;
        break;
    default:
        kind = value->as.object->type == LZ_OBJ_STRING ? LZ_KIND_STRING : 0;
        break;
    }
    return (kind & type->kinds) != 0;
}

/*
 * Fills the inline cache of a member instruction for the class of `instance`, when the member is
 * one the fast paths treat: a plain field (not a link) for what is 0 (read) or 1 (write), a
 * method for 2. The name is looked up with the reference's own lookup, and the checks that depend
 * only on the class and on the code that owns the instruction (visibility, `pub read`) are made
 * here once. Anything else leaves the cache empty, and the reference reports it.
 */
static bool const_is_string(const LzChunk *chunk, size_t index) {
    return index < chunk->constant_count && chunk->constants[index].type == LZ_VAL_OBJECT &&
           chunk->constants[index].as.object->type == LZ_OBJ_STRING;
}

static void ic_fill(LzVM *vm, GeIns *ins, const LzInstance *instance, const LzString *name, int what) {
    const LzClass *cls = instance->cls;
    LzFound hit;
    if (!lz_class_lookup(cls, name, &hit)) {
        return;
    }
    const LzMember *member = hit.member;
    /* A hit from an `extend` block is not in the chain and may be shadowed later: not cached. */
    bool in_chain = false;
    for (const LzClass *c = cls; c != NULL && !in_chain; c = c->parent) {
        in_chain = c == hit.declared_in && member >= c->members && member < c->members + c->member_count;
    }
    if (!in_chain) {
        return;
    }
    const LzCallFrame *frame = &vm->frames[vm->frame_count - 1];
    uint64_t owner = frame->closure != NULL ? frame->closure->function->owner_id : 0;
    bool sees = lz_class_sees(vm, owner, hit.declared_in);
    if (member->vis == LZ_VIS_PRIVATE && !sees) {
        return;
    }
    if (what == 2) {
        if (member->kind != LZ_MEMBER_METHOD || strcmp(name->data, "init") == 0) {
            return;
        }
    } else {
        if (member->kind != LZ_MEMBER_FIELD || member->is_link) {
            return;
        }
        if (what == 1 && member->vis == LZ_VIS_PUB_READ && !sees) {
            return;
        }
    }
    ins->ic_class = cls->id;
    ins->ic_member = member;
}

/*
 * The translated program of a function, made on its first call and kept on the function object,
 * so it lives exactly as long as the function does. NULL when the function's code can't be
 * translated, and the caller then hands that call to the reference interpreter.
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
    /*
     * The code being run changes with every call and return, and these always describe the top
     * frame.
     */
    const LzChunk *chunk = main_chunk;
    GeIns *code = run->program->ins;
    const LzValue *constants = chunk->constants;
    LzTable *globals = lz_chunk_globals(vm, chunk);
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
 * A fused instruction may only run when the fuel covers every instruction it stands for and the
 * stack has room for the deepest point the plain sequence reaches. It charges (FUSED_COMMIT)
 * only after every check has passed, so falling back leaves no trace.
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
            const LzValue *found = builtin_cached(vm, globals, constants, ins);
            if (found == NULL) {
                found = global_ref(globals, constants, ins->a, &ins->hint_a);
                if (found == NULL) {
                    found = builtin_fill(vm, globals, constants, ins); /* not a global: maybe a builtin */
                }
            }
            if (GE_UNLIKELY(found == NULL || sp >= LZ_STACK_MAX)) {
                goto run_plain; /* undefined, so the reference reports it */
            }
            LzValue value = *found;
            grab(&value);
            stack[sp++] = value;
            pc++;
            break;
        }
        case LZ_OP_SET_GLOBAL: {
            TICK(1);
            LzValue *slot = sp > 0 ? global_store(globals, constants, ins->a, &ins->hint_a) : NULL;
            if (GE_UNLIKELY(slot == NULL)) {
                goto run_plain;
            }
            /*
             * The table takes over the stack's reference, which is the reference's retain and
             * pop.
             */
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
        case LZ_OP_ADD: {
            TICK(1);
            LzValue out = GE_NIL_INIT;
            if (GE_UNLIKELY(sp < 2 || !arith(LZ_OP_ADD, &stack[sp - 2], &stack[sp - 1], &out))) {
                if (sp >= 2 && lz_value_is_text(&stack[sp - 2]) && lz_value_is_text(&stack[sp - 1])) {
                    /*
                     * Two strings: the reference's own concatenation, which charges the fuel for
                     * the copy and reports a heap that is full. Nothing changes on a failure.
                     */
                    SYNC_OUT();
                    status = lz_arith_binary(vm, LZ_OP_ADD, &stack[sp - 2], &stack[sp - 1], &out);
                    SYNC_IN();
                    if (status != LZ_OK) {
                        goto fail;
                    }
                    LzValue left = stack[sp - 2];
                    LzValue right = stack[sp - 1];
                    stack[sp - 2] = out;
                    sp--;
                    drop(vm, &right);
                    drop(vm, &left);
                    pc++;
                    break;
                }
                goto run_plain;
            }
            stack[sp - 2] = out;
            sp--;
            pc++;
            break;
        }
            ARITH_CASE(LZ_OP_SUBTRACT)
            ARITH_CASE(LZ_OP_MULTIPLY)
            ARITH_CASE(LZ_OP_DIVIDE)
            ARITH_CASE(LZ_OP_FLOOR_DIVIDE)
            ARITH_CASE(LZ_OP_MODULO)

#define BITWISE_CASE(OPC)                                                                          \
    case OPC: {                                                                                    \
        TICK(1);                                                                                   \
        LzValue out = GE_NIL_INIT;                                                                 \
        if (GE_UNLIKELY(sp < 2 || !bitwise(OPC, &stack[sp - 2], &stack[sp - 1], &out))) {          \
            goto run_plain;                                                                        \
        }                                                                                          \
        stack[sp - 2] = out;                                                                       \
        sp--;                                                                                      \
        pc++;                                                                                      \
        break;                                                                                     \
    }
            BITWISE_CASE(LZ_OP_BIT_AND)
            BITWISE_CASE(LZ_OP_BIT_OR)
            BITWISE_CASE(LZ_OP_BIT_XOR)
            BITWISE_CASE(LZ_OP_SHL)
            BITWISE_CASE(LZ_OP_SHR)

        case LZ_OP_BIT_NOT:
            TICK(1);
            if (GE_UNLIKELY(sp < 1 || stack[sp - 1].type != LZ_VAL_INT)) {
                goto run_plain; /* not an int, so the reference's error */
            }
            stack[sp - 1].as.integer = ~stack[sp - 1].as.integer;
            pc++;
            break;

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
                /*
                 * A dict with an int or string key that is there. The lookup is the dict's own
                 * (hash and key comparison included). A missing key is the reference's, which
                 * words the error.
                 */
                if (sp >= 2 && lz_value_is_dict(&stack[sp - 2]) &&
                    (stack[sp - 1].type == LZ_VAL_INT ||
                     (stack[sp - 1].type == LZ_VAL_OBJECT && stack[sp - 1].as.object->type == LZ_OBJ_STRING))) {
                    const LzValue *stored = lz_dict_get(lz_value_as_dict(&stack[sp - 2]), &stack[sp - 1]);
                    if (stored != NULL) {
                        LzValue found = *stored;
                        grab(&found); /* keep it alive across the releases, which may free the dict */
                        LzValue key = stack[sp - 1];
                        LzValue holder = stack[sp - 2];
                        stack[sp - 2] = found;
                        sp--;
                        drop(vm, &key);
                        drop(vm, &holder);
                        pc++;
                        break;
                    }
                }
                goto run_plain;
            }
            LzValue item = array->items[position];
            grab(&item); /* keep it alive across the release, which may free the array */
            LzObject *holder = stack[sp - 2].as.object;
            stack[sp - 2] = item;
            sp--; /* the index is an int, so there's nothing to release */
            drop_object(vm, holder);
            pc++;
            break;
        }
        case LZ_OP_SET_INDEX: {
            TICK(1);
            LzArray *array = NULL;
            size_t position = 0;
            if (GE_UNLIKELY(sp < 3 || !array_position(&stack[sp - 3], &stack[sp - 2], &array, &position))) {
                if (sp >= 3 && lz_value_is_dict(&stack[sp - 3])) {
                    /* The reference's own store, which checks the key, the types, cycles and fuel. */
                    SYNC_OUT();
                    status = lz_dict_set(vm, lz_value_as_dict(&stack[sp - 3]), &stack[sp - 2], &stack[sp - 1]);
                    SYNC_IN();
                    if (status != LZ_OK) {
                        goto fail;
                    }
                    LzValue value = stack[--sp];
                    LzValue key = stack[--sp];
                    LzValue holder = stack[--sp];
                    drop(vm, &value);
                    drop(vm, &key);
                    drop(vm, &holder);
                    pc++;
                    break;
                }
                goto run_plain;
            }
            const LzValue *value = &stack[sp - 1];
            /*
             * Only the common case of scalar over scalar. Arrays inside arrays need holder
             * bookkeeping and a cycle check, and strings need refcounts, so those are the
             * reference's.
             */
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
                goto run_plain; /* overflow, or not a number, so the reference's error */
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
                    /* Exactly the arguments the function wants, so the frame starts right here. */
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
                    globals = lz_chunk_globals(vm, chunk);
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
            if (callee_object->type == LZ_OBJ_BOUND || callee_object->type == LZ_OBJ_CLASS) {
                /*
                 * A method tied to its receiver enters a frame like a closure, and a class
                 * builds its object.
                 */
                bool entered = false;
                SYNC_OUT();
                status = lz_vm_call(vm, argc, pc + 1, ins->offset, &entered);
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
            if (GE_UNLIKELY(callee_object->type != LZ_OBJ_NATIVE)) {
                goto run_plain;
            }
            /*
             * What the reference's call does. The native sees the stack as it is, and the result
             * replaces the callee and its arguments. On failure nothing is popped.
             */
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
                /* No variable of this frame was captured, so drop the frame and keep the result. */
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
            globals = lz_chunk_globals(vm, chunk);
            nt_count = 0;
            goto native_try;
        }

        case GE_OP_BIN: { /* [load a] [load b] ARITH [store] */
            FUSED_GUARD(2);
            const LzValue *a = load_operand(globals, constants, stack, base, sp, KIND_A(ins), ins->a, &ins->hint_a);
            const LzValue *b = load_operand(globals, constants, stack, base, sp, KIND_B(ins), ins->b, &ins->hint_b);
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
                LzValue *slot = global_store(globals, constants, ins->c, &ins->hint_c);
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
        case GE_OP_BIN1: { /* [load b] ARITH [store], with the left operand on the stack */
            FUSED_GUARD(1);
            if (GE_UNLIKELY(sp < 1)) {
                goto plain_tick;
            }
            const LzValue *a = &stack[sp - 1];
            const LzValue *b = load_operand(globals, constants, stack, base, sp, KIND_B(ins), ins->b, &ins->hint_b);
            LzValue out = GE_NIL_INIT;
            if (GE_UNLIKELY(b == NULL || !arith(ins->sub, a, b, &out))) {
                goto plain_tick;
            }
            switch (KIND_S(ins)) {
            case GE_ST_NONE:
                stack[sp - 1] = out; /* the left operand is a number, so there's nothing to release */
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
                LzValue *slot = global_store(globals, constants, ins->c, &ins->hint_c);
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
            const LzValue *a = load_operand(globals, constants, stack, base, sp, KIND_A(ins), ins->a, &ins->hint_a);
            const LzValue *b = load_operand(globals, constants, stack, base, sp, KIND_B(ins), ins->b, &ins->hint_b);
            bool outcome = false;
            if (GE_UNLIKELY(a == NULL || b == NULL || !compare(ins->sub, a, b, &outcome))) {
                goto plain_tick;
            }
            FUSED_COMMIT();
            pc = outcome ? pc + ins->span : ins->target;
            break;
        }
        case GE_OP_CMPJ1: { /* [load b] CMP JUMP_IF_FALSE POP, with the left operand on the stack */
            FUSED_GUARD(1);
            if (GE_UNLIKELY(sp < 1)) {
                goto plain_tick;
            }
            const LzValue *b = load_operand(globals, constants, stack, base, sp, KIND_B(ins), ins->b, &ins->hint_b);
            bool outcome = false;
            if (GE_UNLIKELY(b == NULL || !compare(ins->sub, &stack[sp - 1], b, &outcome))) {
                goto plain_tick;
            }
            sp--; /* the left operand, a number */
            FUSED_COMMIT();
            pc = outcome ? pc + ins->span : ins->target;
            break;
        }
        case GE_OP_INDEX: { /* [load a] [load b] INDEX, where the array is borrowed and not copied */
            FUSED_GUARD(2);
            const LzValue *a = load_operand(globals, constants, stack, base, sp, KIND_A(ins), ins->a, &ins->hint_a);
            const LzValue *b = load_operand(globals, constants, stack, base, sp, KIND_B(ins), ins->b, &ins->hint_b);
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
        case GE_OP_INDEX1: { /* [load b] INDEX, with the array on the stack */
            FUSED_GUARD(1);
            if (GE_UNLIKELY(sp < 1)) {
                goto plain_tick;
            }
            const LzValue *b = load_operand(globals, constants, stack, base, sp, KIND_B(ins), ins->b, &ins->hint_b);
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

        case LZ_OP_GET_FIELD_W:
        case LZ_OP_SET_FIELD:
        case LZ_OP_METHOD_LOOKUP:
        case GE_OP_DELEGATE:
            /*
             * Instructions that stay marked for the reference (the JIT keys on that mark) but
             * that the interpreter can finish itself when every check passes. Each case checks
             * before it charges or changes anything, and anything else is plain_tick.
             */
            switch (ins->plain) {
            case LZ_OP_CHECK_TYPE:
            case LZ_OP_CHECK_RETURN:
                if (GE_UNLIKELY(sp == 0 || ins->a >= chunk->type_count ||
                                !kind_accepted(&chunk->types[ins->a], &stack[sp - 1]))) {
                    goto plain_tick;
                }
                TICK(1);
                pc++;
                continue;
            case LZ_OP_CHECK_PARAM:
                if (GE_UNLIKELY(base + ins->a >= sp || ins->b >= chunk->type_count ||
                                !kind_accepted(&chunk->types[ins->b], &stack[base + ins->a]))) {
                    goto plain_tick;
                }
                TICK(1);
                pc++;
                continue;
            case LZ_OP_SWAP: {
                if (GE_UNLIKELY(sp < 2)) {
                    goto plain_tick;
                }
                TICK(1);
                LzValue top = stack[sp - 1];
                stack[sp - 1] = stack[sp - 2];
                stack[sp - 2] = top;
                pc++;
                continue;
            }
            case LZ_OP_GET_UPVALUE: {
                const LzClosure *owner = vm->frames[vm->frame_count - 1].closure;
                if (GE_UNLIKELY(owner == NULL || ins->a >= owner->upvalue_count || sp >= LZ_STACK_MAX)) {
                    goto plain_tick;
                }
                TICK(1);
                LzValue value = *owner->upvalues[ins->a]->location;
                grab(&value);
                stack[sp++] = value;
                pc++;
                continue;
            }
            case LZ_OP_SET_UPVALUE: {
                /*
                 * Only a value that cannot close a reference cycle. An object goes to the
                 * reference, where the cycle collector is told about the store.
                 */
                const LzClosure *owner = vm->frames[vm->frame_count - 1].closure;
                if (GE_UNLIKELY(sp == 0 || owner == NULL || ins->a >= owner->upvalue_count ||
                                stack[sp - 1].type == LZ_VAL_OBJECT)) {
                    goto plain_tick;
                }
                TICK(1);
                LzValue *location = owner->upvalues[ins->a]->location;
                LzValue old = *location;
                *location = stack[--sp];
                drop(vm, &old);
                pc++;
                continue;
            }
            case LZ_OP_CLOSURE: {
                if (GE_UNLIKELY(ins->a >= chunk->constant_count ||
                                chunk->constants[ins->a].type != LZ_VAL_OBJECT ||
                                chunk->constants[ins->a].as.object->type != LZ_OBJ_FUNCTION)) {
                    goto plain_tick;
                }
                TICK(1);
                SYNC_OUT();
                status = lz_vm_make_closure(vm, (LzFunction *)chunk->constants[ins->a].as.object);
                SYNC_IN();
                if (status != LZ_OK) {
                    goto fail;
                }
                pc++;
                continue;
            }
            case LZ_OP_GET_FIELD:
            case LZ_OP_GET_FIELD_W:
            case LZ_OP_GET_FIELD_KEEP: {
                if (GE_UNLIKELY(sp == 0 || !lz_value_is_instance(&stack[sp - 1]))) {
                    goto plain_tick;
                }
                LzInstance *object = lz_value_as_instance(&stack[sp - 1]);
                if (GE_UNLIKELY(object->cls->id != ins->ic_class)) {
                    if (const_is_string(chunk, ins->a)) {
                        ic_fill(vm, ins, object, (const LzString *)chunk->constants[ins->a].as.object,
                                ins->plain == LZ_OP_GET_FIELD ? 0 : 1);
                    }
                    if (object->cls->id != ins->ic_class) {
                        goto plain_tick;
                    }
                }
                const LzMember *member = ins->ic_member;
                bool keep = ins->plain == LZ_OP_GET_FIELD_KEEP;
                if (GE_UNLIKELY(((object->unset >> member->slot) & 1u) != 0 || (keep && sp >= LZ_STACK_MAX))) {
                    goto plain_tick;
                }
                TICK(1);
                LzValue field = object->fields[member->slot];
                grab(&field);
                if (keep) {
                    stack[sp++] = field;
                } else {
                    LzValue holder = stack[sp - 1];
                    stack[sp - 1] = field;
                    drop(vm, &holder);
                }
                pc++;
                continue;
            }
            case LZ_OP_SET_FIELD: {
                if (GE_UNLIKELY(sp < 2 || !lz_value_is_instance(&stack[sp - 2]))) {
                    goto plain_tick;
                }
                LzInstance *object = lz_value_as_instance(&stack[sp - 2]);
                if (GE_UNLIKELY(object->cls->id != ins->ic_class)) {
                    if (const_is_string(chunk, ins->a)) {
                        ic_fill(vm, ins, object, (const LzString *)chunk->constants[ins->a].as.object, 1);
                    }
                    if (object->cls->id != ins->ic_class) {
                        goto plain_tick;
                    }
                }
                /*
                 * Only a value that the declared type accepts by kind and that holds no container
                 * (nil, bool, int, float, string), so there is no cycle check and no fuel to
                 * charge. The rest is the reference's.
                 */
                const LzMember *member = ins->ic_member;
                if (GE_UNLIKELY(!kind_accepted(&member->type, &stack[sp - 1]))) {
                    goto plain_tick;
                }
                TICK(1);
                LzValue old = object->fields[member->slot];
                lz_drop_container_holder(&old);
                object->fields[member->slot] = stack[sp - 1];
                object->unset &= ~((uint64_t)1 << member->slot);
                drop(vm, &old);
                LzValue holder = stack[sp - 2];
                sp -= 2;
                drop(vm, &holder);
                pc++;
                continue;
            }
            case LZ_OP_METHOD_LOOKUP: {
                if (GE_UNLIKELY(sp < 2 || !lz_value_is_instance(&stack[sp - 2]))) {
                    goto plain_tick;
                }
                const LzInstance *object = lz_value_as_instance(&stack[sp - 2]);
                if (GE_UNLIKELY(object->cls->id != ins->ic_class)) {
                    /* Three bytes: the name constant and the mode. The translator's operands do not cover them. */
                    size_t name_index = ((size_t)chunk->code[ins->offset + 1] << 8) | chunk->code[ins->offset + 2];
                    if (const_is_string(chunk, name_index)) {
                        ic_fill(vm, ins, object, (const LzString *)chunk->constants[name_index].as.object, 2);
                    }
                    if (object->cls->id != ins->ic_class) {
                        goto plain_tick;
                    }
                }
                TICK(1);
                LzValue callee = ins->ic_member->value;
                grab(&callee);
                LzValue fallback = stack[sp - 1];
                stack[sp - 1] = callee;
                drop(vm, &fallback);
                pc++;
                continue;
            }
            default:
                goto plain_tick;
            }
        default:
            goto plain_tick;
        }
        continue;

    /* A frame was just pushed for a call that needed the slow path, so start running it. */
    enter_callee : {
        const LzCallFrame *frame = &vm->frames[vm->frame_count - 1];
        LzFunction *function = frame->closure->function;
        GeProgram *program = function_program(function, run);
        if (program == NULL) {
            /* Code GE can't translate, so the reference interpreter runs this call. */
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
        globals = lz_chunk_globals(vm, chunk);
        pc = 0;
        nt_count = 1;
        goto native_try;
    }

    /*
     * Hot code runs as machine code (ge_jit.c). It returns here when it finishes, hands an
     * instruction back, or fails, and the stack and frames are always in the interpreter's form.
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
            globals = lz_chunk_globals(vm, chunk);
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
        /* BAIL means the interpreter runs this instruction. Give up on code that keeps bailing. */
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
