#include "lizard/vm.h"

#include "lizard/arith.h"
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

static LzStatus binary_op(LzVM *vm, LzOpCode op) {
    if (vm->stack_count < 2) {
        return malformed(vm, "stack underflow");
    }

    LzValue result = lz_value_nil();
    const LzValue *left = &vm->stack[vm->stack_count - 2];
    const LzValue *right = &vm->stack[vm->stack_count - 1];
    bool is_order = op == LZ_OP_LESS || op == LZ_OP_GREATER;
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

    bool same = lz_value_equal(&vm->stack[vm->stack_count - 2], &vm->stack[vm->stack_count - 1]);
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
            status = binary_op(vm, (LzOpCode)opcode);
            break;
        case LZ_OP_EQUAL:
            status = equal(vm);
            break;
        case LZ_OP_NOT:
            status = not_op(vm);
            break;
        case LZ_OP_NEGATE:
            status = negate(vm);
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

LzStatus lz_vm_run(LzVM *vm, const LzChunk *chunk) {
    size_t failed_at = 0;
    vm->error[0] = '\0';
    vm->error_offset = LZ_NO_OFFSET;
    vm->heap.limit_hit = false;

    LzStatus status = execute(vm, chunk, &failed_at);
    if (status != LZ_OK && failed_at < chunk->count) {
        vm->error_offset = chunk->offsets[failed_at];
    }
    lz_vm_reset_stack(vm);
    return status;
}
