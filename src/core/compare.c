#include "lizard/compare.h"

static bool is_number(const LzValue *value) {
    return value->type == LZ_VAL_INT || value->type == LZ_VAL_FLOAT;
}

LzStatus lz_compare_order(LzVM *vm, LzOpCode op, const LzValue *left, const LzValue *right,
                          LzValue *result) {
    if (!is_number(left) || !is_number(right)) {
        return lz_vm_set_error(vm, LZ_ERR_RUNTIME, "operands must be numbers");
    }

    LzOrder order = lz_number_order(left, right);
    switch (op) {
    case LZ_OP_LESS:
        *result = lz_value_bool(order == LZ_ORDER_LESS);
        return LZ_OK;
    case LZ_OP_GREATER:
        *result = lz_value_bool(order == LZ_ORDER_GREATER);
        return LZ_OK;
    case LZ_OP_LESS_EQUAL:
        *result = lz_value_bool(order == LZ_ORDER_LESS || order == LZ_ORDER_EQUAL);
        return LZ_OK;
    case LZ_OP_GREATER_EQUAL:
        *result = lz_value_bool(order == LZ_ORDER_GREATER || order == LZ_ORDER_EQUAL);
        return LZ_OK;
    default:
        return lz_vm_set_error(vm, LZ_ERR_RUNTIME, "unknown comparison operator");
    }
}
