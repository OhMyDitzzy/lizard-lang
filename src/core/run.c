#include "lizard/vm.h"

#include "lizard/arith.h"
#include "lizard/array.h"
#include "lizard/compare.h"
#include "lizard/native.h"

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

static LzStatus type_mismatch(LzVM *vm, const LzType *type, const LzValue *value) {
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

    LzStatus status = lz_vm_set_error(vm, LZ_ERR_TYPE, "type error: expected %s, got %s",
                                      expected.data != NULL ? expected.data : "?",
                                      actual.data != NULL ? actual.data : "?");
    lz_buffer_free(&expected);
    lz_buffer_free(&actual);
    return status;
}

static LzStatus do_call(LzVM *vm, uint8_t argc) {
    if (vm->stack_count < (size_t)argc + 1) {
        return malformed(vm, "stack underflow");
    }

    const LzValue *callee = &vm->stack[vm->stack_count - argc - 1];
    if (callee->type != LZ_VAL_OBJECT || callee->as.object->type != LZ_OBJ_NATIVE) {
        return lz_vm_set_error(vm, LZ_ERR_RUNTIME, "can only call functions");
    }

    /* The result is a new reference owned by this frame until it is pushed. */
    LzValue result = lz_value_nil();
    const LzValue *args = &vm->stack[vm->stack_count - argc];
    LzStatus status = lz_native_call(vm, (const LzNative *)callee->as.object, argc, args, &result);
    if (status != LZ_OK) {
        return status;
    }

    for (size_t i = 0; i < (size_t)argc + 1; i++) {
        pop(vm);
    }
    vm->stack[vm->stack_count++] = result;
    return LZ_OK;
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

static LzStatus execute(LzVM *vm, const LzChunk *chunk, size_t *failed_at) {
    size_t ip = 0;

    for (;;) {
        *failed_at = ip;
        if (ip >= chunk->count) {
            return malformed(vm, "chunk ended without OP_RETURN");
        }

        LzStatus status = lz_vm_charge_fuel(vm, 1);
        if (status != LZ_OK) {
            return status;
        }
        vm->instructions++;

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
        case LZ_OP_CALL:
            if (ip >= chunk->count) {
                status = malformed(vm, "truncated operand");
            } else {
                status = do_call(vm, chunk->code[ip++]);
            }
            break;
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
            status = binary_op(vm, (LzOpCode)opcode);
            break;
        case LZ_OP_GET_LOCAL: {
            uint16_t slot;
            if (!read_u16(chunk, &ip, &slot)) {
                status = malformed(vm, "truncated operand");
            } else if (slot >= vm->stack_count) {
                status = malformed(vm, "local slot out of range");
            } else {
                status = push(vm, vm->stack[slot]);
            }
            break;
        }
        case LZ_OP_SET_LOCAL: {
            uint16_t slot;
            if (!read_u16(chunk, &ip, &slot)) {
                status = malformed(vm, "truncated operand");
            } else if (vm->stack_count == 0 || slot >= vm->stack_count - 1) {
                status = malformed(vm, "local slot out of range");
            } else {
                LzValue moved = vm->stack[--vm->stack_count];
                lz_value_release(&vm->heap, &vm->stack[slot]);
                vm->stack[slot] = moved;
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
        case LZ_OP_CHECK_TYPE: {
            uint16_t index;
            if (!read_u16(chunk, &ip, &index)) {
                status = malformed(vm, "truncated operand");
            } else if (index >= chunk->type_count) {
                status = malformed(vm, "type index out of range");
            } else if (vm->stack_count == 0) {
                status = malformed(vm, "stack underflow");
            } else {
                const LzType *type = &chunk->types[index];
                const LzValue *top = &vm->stack[vm->stack_count - 1];
                bool fits;
                if (lz_value_is_array(top)) {
                    /* An array is checked item by item, and then restricted to the item type. */
                    status = lz_vm_charge_fuel(vm, lz_array_cost(lz_value_as_array(top)->count));
                    fits = (type->kinds & LZ_KIND_ARRAY) != 0 &&
                           lz_array_conform(lz_value_as_array(top), type->element);
                } else {
                    fits = lz_type_matches(type, top);
                }
                if (status == LZ_OK && !fits) {
                    status = type_mismatch(vm, type, top);
                }
            }
            break;
        }
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
            return LZ_OK;
        default:
            status = malformed(vm, "unknown opcode");
            break;
        }

        if (status != LZ_OK) {
            return status;
        }
    }
}

LzStatus lz_vm_eval(LzVM *vm, const LzChunk *chunk, LzValue *result) {
    size_t failed_at = 0;
    vm->error[0] = '\0';
    vm->error_offset = LZ_NO_OFFSET;
    vm->heap.limit_hit = false;

    LzStatus status = execute(vm, chunk, &failed_at);
    if (status != LZ_OK && failed_at < chunk->count) {
        vm->error_offset = chunk->offsets[failed_at];
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

LzStatus lz_vm_run(LzVM *vm, const LzChunk *chunk) {
    return lz_vm_eval(vm, chunk, NULL);
}
