#include "lizard/compare.h"

#include "lizard/class.h"
#include "lizard/link.h"

static bool is_number(const LzValue *value) {
    return value->type == LZ_VAL_INT || value->type == LZ_VAL_FLOAT || value->type == LZ_VAL_BOOL;
}

LzStatus lz_compare_order(LzVM *vm, LzOpCode op, const LzValue *left, const LzValue *right,
                          LzValue *result) {
    if (lz_value_is_instance(left) || lz_value_is_instance(right)) {
        bool handled = false;
        LzStatus status = lz_class_order_operator(vm, op, left, right, result, &handled);
        if (handled) {
            return status;
        }
    }
    if (lz_value_is_link(left) || lz_value_is_link(right)) {
        return lz_vm_link_error(vm);
    }
    if (!is_number(left) || !is_number(right)) {
        return lz_vm_set_error(vm, LZ_ERR_RUNTIME, "operands must be numbers");
    }

    /* Normalize only ordering operands so equality and dict keys stay distinct. */
    LzValue a = *left;
    LzValue b = *right;
    if (a.type == LZ_VAL_BOOL) {
        a = lz_value_int((int64_t)a.as.boolean);
    }
    if (b.type == LZ_VAL_BOOL) {
        b = lz_value_int((int64_t)b.as.boolean);
    }
    LzOrder order = lz_number_order(&a, &b);
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
