#include "lizard/vm.h"

#include "lizard/arith.h"
#include "lizard/array.h"
#include "lizard/compare.h"
#include "lizard/native.h"

#include <stdio.h>

static bool read_u16(const LzChunk *chunk, size_t *ip, uint16_t *out) {
    if (*ip + 2 > chunk->count) {
        return false;
    }
    *out = (uint16_t)((chunk->code[*ip] << 8) | chunk->code[*ip + 1]);
    *ip += 2;
    return true;
}

static LzStatus malformed(LzVM *vm, const char *what) {
    return lz_vm_set_error(vm, LZ_ERR_RUNTIME, "malformed bytecode: %s", what);
}

static LzStatus push(LzVM *vm, LzValue value) {
    if (vm->stack_count >= LZ_STACK_MAX) {
        return lz_vm_set_error(vm, LZ_ERR_STACK_OVERFLOW, "stack overflow");
    }
    lz_value_retain(&value);
    vm->stack[vm->stack_count++] = value;
    return LZ_OK;
}

static void pop(LzVM *vm) {
    vm->stack_count--;
    lz_value_release(&vm->heap, &vm->stack[vm->stack_count]);
}

static LzStatus read_constant(LzVM *vm, const LzChunk *chunk, size_t *ip, const LzValue **out) {
    uint16_t index;
    if (!read_u16(chunk, ip, &index)) {
        return malformed(vm, "truncated operand");
    }
    if (index >= chunk->constant_count) {
        return malformed(vm, "constant index out of range");
    }
    *out = &chunk->constants[index];
    return LZ_OK;
}

static LzStatus read_name(LzVM *vm, const LzChunk *chunk, size_t *ip, LzString **name) {
    const LzValue *constant = NULL;
    LzStatus status = read_constant(vm, chunk, ip, &constant);
    if (status != LZ_OK) {
        return status;
    }
    if (constant->type != LZ_VAL_OBJECT || constant->as.object->type != LZ_OBJ_STRING) {
        return malformed(vm, "name operand is not a string");
    }
    *name = (LzString *)constant->as.object;
    return LZ_OK;
}

/* What a failed check was about, for the message: an argument, a returned value, or nothing. */
typedef enum CheckSubject { CHECK_VALUE, CHECK_PARAM, CHECK_RETURN } CheckSubject;

static void describe_subject(const LzVM *vm, CheckSubject subject, size_t slot, char *out, size_t size) {
    out[0] = '\0';
    const LzCallFrame *frame = &vm->frames[vm->frame_count - 1];
    if (subject == CHECK_VALUE || frame->closure == NULL) {
        return;
    }
    const LzFunction *function = frame->closure->function;
    if (subject == CHECK_PARAM) {
        const char *name = slot < lz_function_slots(function) ? function->param_names[slot]->data : "?";
        (void)snprintf(out, size, "argument '%s' of %s()", name, lz_function_display_name(function));
    } else {
        (void)snprintf(out, size, "return value of %s()", lz_function_display_name(function));
    }
}

static LzStatus type_mismatch(LzVM *vm, const LzType *type, const LzValue *value,
                              CheckSubject subject, size_t slot) {
    LzBuffer expected = {0};
    LzBuffer actual = {0};
    LzWriter expected_out = lz_writer_from_buffer(&expected);
    LzWriter actual_out = lz_writer_from_buffer(&actual);

    lz_type_describe(type, &expected_out);
    if (lz_value_is_array(value)) {
        LzType shown = lz_type_of_value(value);
        lz_type_describe(&shown, &actual_out);
    } else if (type->literal_count > 0) {
        lz_value_describe(value, &actual_out);
    } else {
        (void)lz_writer_write_cstr(&actual_out, lz_kind_name(lz_kind_of(value)));
    }

    char what[160];
    describe_subject(vm, subject, slot, what, sizeof what);
    LzStatus status = lz_vm_set_error(vm, LZ_ERR_TYPE, "type error: %s%sexpected %s, got %s", what,
                                      what[0] != '\0' ? ": " : "",
                                      expected.data != NULL ? expected.data : "?",
                                      actual.data != NULL ? actual.data : "?");
    lz_buffer_free(&expected);
    lz_buffer_free(&actual);
    return status;
}

/*
 * Does the value fit the type? An array is checked item by item and then restricted to
 * the item type, which costs fuel in proportion to its size.
 */
static LzStatus check_value(LzVM *vm, const LzType *type, const LzValue *value, CheckSubject subject,
                            size_t slot) {
    LzStatus status = LZ_OK;
    bool fits;
    if (lz_value_is_array(value)) {
        status = lz_vm_charge_fuel(vm, lz_array_cost(lz_value_as_array(value)->count));
        fits = (type->kinds & LZ_KIND_ARRAY) != 0 && lz_array_conform(lz_value_as_array(value), type->element);
    } else {
        fits = lz_type_matches(type, value);
    }
    if (status == LZ_OK && !fits) {
        status = type_mismatch(vm, type, value, subject, slot);
    }
    return status;
}

static LzStatus array_new(LzVM *vm) {
    LzArray *array = lz_array_new(&vm->heap, 0);
    if (array == NULL) {
        return lz_vm_out_of_memory(vm);
    }
    LzStatus status = push(vm, lz_value_object(&array->base));
    lz_object_release(&vm->heap, &array->base); /* the stack holds its own reference now */
    return status;
}

static LzStatus array_push(LzVM *vm) {
    if (vm->stack_count < 2) {
        return malformed(vm, "stack underflow");
    }
    const LzValue *target = &vm->stack[vm->stack_count - 2];
    if (!lz_value_is_array(target)) {
        return malformed(vm, "array expected");
    }
    LzStatus status = lz_array_push(vm, lz_value_as_array(target), &vm->stack[vm->stack_count - 1]);
    if (status == LZ_OK) {
        pop(vm);
    }
    return status;
}

static LzStatus not_indexable(LzVM *vm, const LzValue *value) {
    return lz_vm_set_error(vm, LZ_ERR_RUNTIME, "cannot index a value of type %s",
                           lz_kind_name(lz_kind_of(value)));
}

static LzStatus index_op(LzVM *vm) {
    if (vm->stack_count < 2) {
        return malformed(vm, "stack underflow");
    }
    const LzValue *container = &vm->stack[vm->stack_count - 2];
    const LzValue *index = &vm->stack[vm->stack_count - 1];
    if (!lz_value_is_array(container)) {
        return not_indexable(vm, container);
    }
    const LzArray *array = lz_value_as_array(container);
    size_t position = 0;
    LzStatus status = lz_array_resolve_index(vm, index, array->count, &position);
    if (status != LZ_OK) {
        return status;
    }
    LzValue item = array->items[position];
    lz_value_retain(&item); /* keep it alive across the pops, which may free the array */
    pop(vm);
    pop(vm);
    vm->stack[vm->stack_count++] = item;
    return LZ_OK;
}

static LzStatus set_index_op(LzVM *vm) {
    if (vm->stack_count < 3) {
        return malformed(vm, "stack underflow");
    }
    const LzValue *container = &vm->stack[vm->stack_count - 3];
    if (!lz_value_is_array(container)) {
        return not_indexable(vm, container);
    }
    LzStatus status = lz_array_set(vm, lz_value_as_array(container),
                                   &vm->stack[vm->stack_count - 2], &vm->stack[vm->stack_count - 1]);
    if (status != LZ_OK) {
        return status;
    }
    pop(vm);
    pop(vm);
    pop(vm);
    return LZ_OK;
}

static LzStatus slice_op(LzVM *vm) {
    if (vm->stack_count < 3) {
        return malformed(vm, "stack underflow");
    }
    const LzValue *container = &vm->stack[vm->stack_count - 3];
    if (!lz_value_is_array(container)) {
        return not_indexable(vm, container);
    }
    LzValue result = lz_value_nil();
    LzStatus status = lz_array_slice(vm, lz_value_as_array(container),
                                     &vm->stack[vm->stack_count - 2],
                                     &vm->stack[vm->stack_count - 1], &result);
    if (status != LZ_OK) {
        return status;
    }
    pop(vm);
    pop(vm);
    pop(vm);
    vm->stack[vm->stack_count++] = result;
    return LZ_OK;
}

static LzStatus array_extend(LzVM *vm) {
    if (vm->stack_count < 2) {
        return malformed(vm, "stack underflow");
    }
    const LzValue *target = &vm->stack[vm->stack_count - 2];
    const LzValue *source = &vm->stack[vm->stack_count - 1];
    if (!lz_value_is_array(target)) {
        return malformed(vm, "array expected");
    }
    if (!lz_value_is_array(source)) {
        return lz_vm_set_error(vm, LZ_ERR_RUNTIME, "cannot spread a value of type %s",
                               lz_kind_name(lz_kind_of(source)));
    }
    const LzArray *items = lz_value_as_array(source);
    LzStatus status = lz_vm_charge_fuel(vm, lz_array_cost(items->count));
    for (size_t i = 0; status == LZ_OK && i < items->count; i++) {
        status = lz_array_push(vm, lz_value_as_array(target), &items->items[i]);
    }
    if (status == LZ_OK) {
        pop(vm);
    }
    return status;
}

static LzStatus binary_op(LzVM *vm, LzOpCode op) {
    if (vm->stack_count < 2) {
        return malformed(vm, "stack underflow");
    }

    LzValue result = lz_value_nil();
    const LzValue *left = &vm->stack[vm->stack_count - 2];
    const LzValue *right = &vm->stack[vm->stack_count - 1];
    bool is_order = op == LZ_OP_LESS || op == LZ_OP_GREATER || op == LZ_OP_LESS_EQUAL ||
                    op == LZ_OP_GREATER_EQUAL;
    LzStatus status = is_order ? lz_compare_order(vm, op, left, right, &result)
                               : lz_arith_binary(vm, op, left, right, &result);
    if (status != LZ_OK) {
        return status;
    }
    pop(vm);
    pop(vm);
    vm->stack[vm->stack_count++] = result;
    return LZ_OK;
}

static LzStatus equal(LzVM *vm) {
    if (vm->stack_count < 2) {
        return malformed(vm, "stack underflow");
    }

    const LzValue *left = &vm->stack[vm->stack_count - 2];
    const LzValue *right = &vm->stack[vm->stack_count - 1];

    bool same = false;
    LzStatus status = lz_vm_values_equal(vm, left, right, &same);
    if (status != LZ_OK) {
        return status;
    }
    pop(vm);
    pop(vm);
    vm->stack[vm->stack_count++] = lz_value_bool(same);
    return LZ_OK;
}

static LzStatus not_op(LzVM *vm) {
    if (vm->stack_count < 1) {
        return malformed(vm, "stack underflow");
    }

    bool falsy = !lz_value_is_truthy(&vm->stack[vm->stack_count - 1]);
    pop(vm);
    vm->stack[vm->stack_count++] = lz_value_bool(falsy);
    return LZ_OK;
}

static LzStatus negate(LzVM *vm) {
    if (vm->stack_count < 1) {
        return malformed(vm, "stack underflow");
    }

    LzValue result = lz_value_nil();
    LzStatus status = lz_arith_negate(vm, &vm->stack[vm->stack_count - 1], &result);
    if (status != LZ_OK) {
        return status;
    }
    pop(vm);
    vm->stack[vm->stack_count++] = result;
    return LZ_OK;
}

static LzStatus bit_not(LzVM *vm) {
    if (vm->stack_count < 1) {
        return malformed(vm, "stack underflow");
    }

    LzValue result = lz_value_nil();
    LzStatus status = lz_arith_bit_not(vm, &vm->stack[vm->stack_count - 1], &result);
    if (status != LZ_OK) {
        return status;
    }
    pop(vm);
    vm->stack[vm->stack_count++] = result;
    return LZ_OK;
}

/*
 * Runs the one instruction at byte offset *ip_io and advances it past the
 * instruction. It does no accounting (fuel, vm->instructions): the loop that
 * calls it does that, so a faster engine can spend fuel its own way and still
 * reuse every semantic.
 *
 * OP_RETURN is reported by setting *ip_io to SIZE_MAX, which the loop already
 * has to treat as "no more instructions": the hot path pays nothing for it.
 * Marked always_inline so the reference loop below keeps its speed.
 */
#if defined(__GNUC__) || defined(__clang__)
#define LZ_FORCE_INLINE static inline __attribute__((always_inline))
#else
#define LZ_FORCE_INLINE static inline
#endif

LZ_FORCE_INLINE LzStatus exec_one(LzVM *vm, const LzChunk **chunk_io, size_t *ip_io,
                                  bool frames_ok) {
    const LzChunk *chunk = *chunk_io;
    size_t ip = *ip_io;
    LzStatus status = LZ_OK;
    if (ip >= chunk->count) {
        return malformed(vm, "chunk ended without OP_RETURN");
    }

    const size_t start = ip;
    uint8_t opcode = chunk->code[ip++];
    switch (opcode) {
    case LZ_OP_CONSTANT: {
        const LzValue *constant = NULL;
        status = read_constant(vm, chunk, &ip, &constant);
        if (status == LZ_OK) {
            status = push(vm, *constant);
        }
        break;
    }
    case LZ_OP_NIL:
        status = push(vm, lz_value_nil());
        break;
    case LZ_OP_TRUE:
        status = push(vm, lz_value_bool(true));
        break;
    case LZ_OP_FALSE:
        status = push(vm, lz_value_bool(false));
        break;
    case LZ_OP_POP:
        if (vm->stack_count == 0) {
            status = malformed(vm, "stack underflow");
        } else {
            pop(vm);
        }
        break;
    case LZ_OP_GET_GLOBAL: {
        LzString *name;
        status = read_name(vm, chunk, &ip, &name);
        if (status != LZ_OK) {
            break;
        }
        const LzValue *value = lz_vm_lookup(vm, name);
        if (value == NULL) {
            status = lz_vm_set_error(vm, LZ_ERR_RUNTIME, "undefined variable '%s'", name->data);
        } else {
            status = push(vm, *value);
        }
        break;
    }
    case LZ_OP_DEFINE_GLOBAL: {
        LzString *name;
        status = read_name(vm, chunk, &ip, &name);
        if (status != LZ_OK) {
            break;
        }
        if (vm->stack_count == 0) {
            status = malformed(vm, "stack underflow");
        } else if (!lz_vm_define_global(vm, name, vm->stack[vm->stack_count - 1])) {
            status = lz_vm_out_of_memory(vm);
        } else {
            pop(vm);
        }
        break;
    }
    case LZ_OP_CALL: {
        if (ip >= chunk->count) {
            status = malformed(vm, "truncated operand");
            break;
        }
        size_t argc = chunk->code[ip++];
        if (!frames_ok && vm->stack_count > argc && lz_value_is_closure(&vm->stack[vm->stack_count - argc - 1])) {
            status = malformed(vm, "a single step cannot enter a function");
            break;
        }
        bool entered = false;
        status = lz_vm_call(vm, argc, ip, (uint32_t)start, &entered);
        if (entered) {
            *chunk_io = chunk = vm->frames[vm->frame_count - 1].chunk;
            ip = 0;
        }
        break;
    }
    case LZ_OP_CALL_EX: {
        uint16_t names;
        if (ip >= chunk->count) {
            status = malformed(vm, "truncated operand");
            break;
        }
        size_t named = chunk->code[ip++];
        if (!read_u16(chunk, &ip, &names)) {
            status = malformed(vm, "truncated operand");
        } else if (!frames_ok) {
            status = malformed(vm, "a single step cannot enter a function");
        } else {
            bool entered = false;
            status = lz_vm_call_ex(vm, chunk, named, names, ip, (uint32_t)start, &entered);
            if (entered) {
                *chunk_io = chunk = vm->frames[vm->frame_count - 1].chunk;
                ip = 0;
            }
        }
        break;
    }
    case LZ_OP_ADD:
    case LZ_OP_SUBTRACT:
    case LZ_OP_MULTIPLY:
    case LZ_OP_DIVIDE:
    case LZ_OP_FLOOR_DIVIDE:
    case LZ_OP_MODULO:
    case LZ_OP_LESS:
    case LZ_OP_GREATER:
    case LZ_OP_LESS_EQUAL:
    case LZ_OP_GREATER_EQUAL:
    case LZ_OP_BIT_AND:
    case LZ_OP_BIT_OR:
    case LZ_OP_BIT_XOR:
    case LZ_OP_SHL:
    case LZ_OP_SHR:
        status = binary_op(vm, (LzOpCode)opcode);
        break;
    case LZ_OP_GET_LOCAL: {
        uint16_t slot;
        if (!read_u16(chunk, &ip, &slot)) {
            status = malformed(vm, "truncated operand");
        } else if (vm->frames[vm->frame_count - 1].base + slot >= vm->stack_count) {
            status = malformed(vm, "local slot out of range");
        } else {
            status = push(vm, vm->stack[vm->frames[vm->frame_count - 1].base + slot]);
        }
        break;
    }
    case LZ_OP_SET_LOCAL: {
        uint16_t slot;
        if (!read_u16(chunk, &ip, &slot)) {
            status = malformed(vm, "truncated operand");
        } else if (vm->stack_count == 0 ||
                   vm->frames[vm->frame_count - 1].base + slot >= vm->stack_count - 1) {
            status = malformed(vm, "local slot out of range");
        } else {
            size_t index = vm->frames[vm->frame_count - 1].base + slot;
            LzValue moved = vm->stack[--vm->stack_count];
            lz_value_release(&vm->heap, &vm->stack[index]);
            vm->stack[index] = moved;
        }
        break;
    }
    case LZ_OP_SET_GLOBAL: {
        LzString *name;
        status = read_name(vm, chunk, &ip, &name);
        if (status != LZ_OK) {
            break;
        }
        if (vm->stack_count == 0) {
            status = malformed(vm, "stack underflow");
        } else if (lz_table_get(&vm->globals, name) == NULL) {
            status = lz_vm_set_error(vm, LZ_ERR_RUNTIME, "undefined variable '%s'", name->data);
        } else if (!lz_vm_define_global(vm, name, vm->stack[vm->stack_count - 1])) {
            status = lz_vm_out_of_memory(vm);
        } else {
            pop(vm);
        }
        break;
    }
    case LZ_OP_JUMP:
    case LZ_OP_JUMP_IF_FALSE:
    case LZ_OP_JUMP_IF_TRUE: {
        uint16_t distance;
        if (!read_u16(chunk, &ip, &distance)) {
            status = malformed(vm, "truncated operand");
        } else if (ip + distance > chunk->count) {
            status = malformed(vm, "jump out of range");
        } else if (opcode == LZ_OP_JUMP) {
            ip += distance;
        } else if (vm->stack_count == 0) {
            status = malformed(vm, "stack underflow");
        } else if (lz_value_is_truthy(&vm->stack[vm->stack_count - 1]) ==
                   (opcode == LZ_OP_JUMP_IF_TRUE)) {
            ip += distance;
        }
        break;
    }
    case LZ_OP_LOOP: {
        uint16_t distance;
        if (!read_u16(chunk, &ip, &distance)) {
            status = malformed(vm, "truncated operand");
        } else if (distance > ip) {
            status = malformed(vm, "jump out of range");
        } else {
            ip -= distance;
        }
        break;
    }
    case LZ_OP_EQUAL:
        status = equal(vm);
        break;
    case LZ_OP_NOT:
        status = not_op(vm);
        break;
    case LZ_OP_NEGATE:
        status = negate(vm);
        break;
    case LZ_OP_BIT_NOT:
        status = bit_not(vm);
        break;
    case LZ_OP_CHECK_TYPE: {
        uint16_t index;
        if (!read_u16(chunk, &ip, &index)) {
            status = malformed(vm, "truncated operand");
        } else if (index >= chunk->type_count) {
            status = malformed(vm, "type index out of range");
        } else if (vm->stack_count == 0) {
            status = malformed(vm, "stack underflow");
        } else {
            status = check_value(vm, &chunk->types[index], &vm->stack[vm->stack_count - 1],
                                 CHECK_VALUE, 0);
        }
        break;
    }
    case LZ_OP_CHECK_PARAM: {
        uint16_t slot;
        uint16_t index;
        if (!read_u16(chunk, &ip, &slot) || !read_u16(chunk, &ip, &index)) {
            status = malformed(vm, "truncated operand");
        } else if (index >= chunk->type_count) {
            status = malformed(vm, "type index out of range");
        } else if (vm->frames[vm->frame_count - 1].base + slot >= vm->stack_count) {
            status = malformed(vm, "local slot out of range");
        } else {
            status = check_value(vm, &chunk->types[index],
                                 &vm->stack[vm->frames[vm->frame_count - 1].base + slot],
                                 CHECK_PARAM, slot);
        }
        break;
    }
    case LZ_OP_CHECK_RETURN: {
        uint16_t index;
        if (!read_u16(chunk, &ip, &index)) {
            status = malformed(vm, "truncated operand");
        } else if (index >= chunk->type_count) {
            status = malformed(vm, "type index out of range");
        } else if (vm->stack_count == 0) {
            status = malformed(vm, "stack underflow");
        } else {
            status = check_value(vm, &chunk->types[index], &vm->stack[vm->stack_count - 1],
                                 CHECK_RETURN, 0);
        }
        break;
    }
    case LZ_OP_JUMP_IF_SUPPLIED: {
        uint16_t parameter;
        uint16_t distance;
        if (!read_u16(chunk, &ip, &parameter) || !read_u16(chunk, &ip, &distance)) {
            status = malformed(vm, "truncated operand");
        } else if (ip + distance > chunk->count) {
            status = malformed(vm, "jump out of range");
        } else if (parameter >= 64 ||
                   ((vm->frames[vm->frame_count - 1].supplied >> parameter) & 1u) != 0) {
            ip += distance;
        }
        break;
    }
    case LZ_OP_CLOSURE: {
        const LzValue *constant = NULL;
        status = read_constant(vm, chunk, &ip, &constant);
        if (status != LZ_OK) {
            break;
        }
        if (constant->type != LZ_VAL_OBJECT || constant->as.object->type != LZ_OBJ_FUNCTION) {
            status = malformed(vm, "closure operand is not a function");
        } else {
            status = lz_vm_make_closure(vm, (LzFunction *)constant->as.object);
        }
        break;
    }
    case LZ_OP_GET_UPVALUE:
    case LZ_OP_SET_UPVALUE: {
        uint16_t index;
        const LzClosure *closure = vm->frames[vm->frame_count - 1].closure;
        if (!read_u16(chunk, &ip, &index)) {
            status = malformed(vm, "truncated operand");
        } else if (closure == NULL || index >= closure->upvalue_count) {
            status = malformed(vm, "upvalue index out of range");
        } else if (opcode == LZ_OP_GET_UPVALUE) {
            status = push(vm, *closure->upvalues[index]->location);
        } else if (vm->stack_count == 0) {
            status = malformed(vm, "stack underflow");
        } else {
            LzUpvalue *upvalue = closure->upvalues[index];
            LzValue *location = upvalue->location;
            LzValue moved = vm->stack[--vm->stack_count];
            LzValue old = *location;
            *location = moved;
            lz_value_release(&vm->heap, &old);
            if (moved.type == LZ_VAL_OBJECT) {
                /* The GE interpreter delegates this instruction to here, and both JIT back-ends
                 * send every object store here too: they inline only what cannot close a cycle. */
                lz_skink_note_store(vm, upvalue);
            }
        }
        break;
    }
    case LZ_OP_CLOSE_UPVALUE:
        if (vm->stack_count == 0) {
            status = malformed(vm, "stack underflow");
        } else {
            lz_vm_close_upvalues(vm, vm->stack_count - 1);
            pop(vm);
            lz_skink_run(vm);
        }
        break;
    case LZ_OP_GET_SELF: {
        const LzClosure *closure = vm->frames[vm->frame_count - 1].closure;
        if (closure == NULL) {
            status = malformed(vm, "no running function");
        } else {
            status = push(vm, lz_value_object((LzObject *)&closure->base));
        }
        break;
    }
    case LZ_OP_SWAP:
        if (vm->stack_count < 2) {
            status = malformed(vm, "stack underflow");
        } else {
            LzValue top = vm->stack[vm->stack_count - 1];
            vm->stack[vm->stack_count - 1] = vm->stack[vm->stack_count - 2];
            vm->stack[vm->stack_count - 2] = top;
        }
        break;
    case LZ_OP_ARRAY_EXTEND:
        status = array_extend(vm);
        break;
    case LZ_OP_ARRAY_NEW:
        status = array_new(vm);
        break;
    case LZ_OP_ARRAY_PUSH:
        status = array_push(vm);
        break;
    case LZ_OP_INDEX:
        status = index_op(vm);
        break;
    case LZ_OP_SET_INDEX:
        status = set_index_op(vm);
        break;
    case LZ_OP_SLICE:
        status = slice_op(vm);
        break;
    case LZ_OP_RETURN:
        if (vm->frame_count <= 1) {
            ip = SIZE_MAX;
        } else if (!frames_ok) {
            status = malformed(vm, "a single step cannot leave a function");
        } else {
            status = lz_vm_return(vm);
            if (status == LZ_OK) {
                const LzCallFrame *caller = &vm->frames[vm->frame_count - 1];
                *chunk_io = chunk = caller->chunk;
                ip = caller->ip;
            }
        }
        break;
    default:
        status = malformed(vm, "unknown opcode");
        break;
    }

    *ip_io = ip;
    return status;
}

LzStatus lz_vm_exec_one(LzVM *vm, const LzChunk *chunk, size_t *ip, bool *halted) {
    size_t next = *ip;
    const LzChunk *current = chunk;
    LzStatus status = exec_one(vm, &current, &next, false);
    *halted = next == SIZE_MAX;
    *ip = *halted ? *ip + 1 : next;
    return status;
}

static LzStatus execute(LzVM *vm, const LzChunk *main_chunk, size_t *failed_at, void *context) {
    (void)context;
    const LzChunk *chunk = main_chunk;
    size_t ip = 0;

    for (;;) {
        *failed_at = ip;
        if (ip >= chunk->count) {
            return ip == SIZE_MAX ? LZ_OK : malformed(vm, "chunk ended without OP_RETURN");
        }

        LzStatus status = lz_vm_charge_fuel(vm, 1);
        if (status != LZ_OK) {
            return status;
        }
        vm->instructions++;

        status = exec_one(vm, &chunk, &ip, true);
        if (status != LZ_OK) {
            return status;
        }
    }
}

LzStatus lz_vm_run_until(LzVM *vm, size_t depth, size_t *failed_at) {
    const LzCallFrame *top = &vm->frames[vm->frame_count - 1];
    const LzChunk *chunk = top->chunk;
    size_t ip = top->ip;

    while (vm->frame_count > depth) {
        *failed_at = ip;
        if (ip >= chunk->count) {
            return malformed(vm, "chunk ended without OP_RETURN");
        }
        LzStatus status = lz_vm_charge_fuel(vm, 1);
        if (status != LZ_OK) {
            return status;
        }
        vm->instructions++;
        status = exec_one(vm, &chunk, &ip, true);
        if (status != LZ_OK) {
            return status;
        }
    }
    return LZ_OK;
}

LzStatus lz_vm_eval_with(LzVM *vm, const LzChunk *chunk, LzValue *result, LzExecuteFn run,
                         void *context) {
    size_t failed_at = 0;
    vm->error[0] = '\0';
    vm->error_offset = LZ_NO_OFFSET;
    vm->native_depth = 0;
    vm->trace_fixed = false;
    vm->heap.limit_hit = false;

    /* Frame 0 is the script itself: no closure, locals from the bottom of the stack. */
    vm->frame_count = 1;
    vm->frames[0] = (LzCallFrame){NULL, chunk, 0, 0, LZ_ALL_SUPPLIED, LZ_NO_OFFSET};

    LzStatus status = run(vm, chunk, &failed_at, context);
    if (status != LZ_OK) {
        /* The failing instruction belongs to the frame that was on top when it failed. */
        const LzChunk *failing = vm->frame_count > 0 ? vm->frames[vm->frame_count - 1].chunk : chunk;
        /* A failure inside a callback has already recorded its own offset. */
        if (failed_at < failing->count && vm->error_offset == LZ_NO_OFFSET) {
            vm->error_offset = failing->offsets[failed_at];
        }
        if (!vm->trace_fixed) {
            lz_vm_capture_trace(vm);
        }
    }
    if (status == LZ_OK && result != NULL) {
        if (vm->stack_count > 0) {
            *result = vm->stack[vm->stack_count - 1];
            lz_value_retain(result);
        } else {
            *result = lz_value_nil();
        }
    }
    lz_vm_reset_stack(vm);
    return status;
}

LzStatus lz_vm_eval(LzVM *vm, const LzChunk *chunk, LzValue *result) {
    return lz_vm_eval_with(vm, chunk, result, execute, NULL);
}

LzStatus lz_vm_run(LzVM *vm, const LzChunk *chunk) {
    return lz_vm_eval(vm, chunk, NULL);
}
