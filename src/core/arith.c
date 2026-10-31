#include "lizard/arith.h"

#include "lizard/array.h"

#include <math.h>

static bool is_number(const LzValue *value) {
    return value->type == LZ_VAL_INT || value->type == LZ_VAL_FLOAT;
}

static double as_double(const LzValue *value) {
    return value->type == LZ_VAL_INT ? (double)value->as.integer : value->as.number;
}

#if defined(__GNUC__) && !defined(LZ_PORTABLE_OVERFLOW)

static bool add_overflows(int64_t a, int64_t b, int64_t *out) {
    return __builtin_add_overflow(a, b, out);
}

static bool subtract_overflows(int64_t a, int64_t b, int64_t *out) {
    return __builtin_sub_overflow(a, b, out);
}

static bool multiply_overflows(int64_t a, int64_t b, int64_t *out) {
    return __builtin_mul_overflow(a, b, out);
}

#else

static bool add_overflows(int64_t a, int64_t b, int64_t *out) {
    if ((b > 0 && a > INT64_MAX - b) || (b < 0 && a < INT64_MIN - b)) {
        return true;
    }
    *out = a + b;
    return false;
}

static bool subtract_overflows(int64_t a, int64_t b, int64_t *out) {
    if ((b < 0 && a > INT64_MAX + b) || (b > 0 && a < INT64_MIN + b)) {
        return true;
    }
    *out = a - b;
    return false;
}

static bool multiply_overflows(int64_t a, int64_t b, int64_t *out) {
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

static bool is_string(const LzValue *value) {
    return value->type == LZ_VAL_OBJECT && value->as.object->type == LZ_OBJ_STRING;
}

static LzStatus concat(LzVM *vm, const LzValue *left, const LzValue *right, LzValue *result) {
    /* Copying is real work: one fuel unit per 64 bytes keeps fuel a bound on time. */
    LzStatus charged = lz_vm_charge_fuel(vm, (((const LzString *)left->as.object)->length +
                                              ((const LzString *)right->as.object)->length) /
                                                 64);
    if (charged != LZ_OK) {
        return charged;
    }
    LzString *joined = lz_string_concat(&vm->heap, (const LzString *)left->as.object,
                                        (const LzString *)right->as.object);
    if (joined == NULL) {
        return lz_vm_out_of_memory(vm);
    }
    *result = lz_value_object(&joined->base);
    return LZ_OK;
}

static LzStatus division_by_zero(LzVM *vm) {
    return lz_vm_set_error(vm, LZ_ERR_RUNTIME, "division by zero");
}

static LzStatus integer_floor_divide(LzVM *vm, int64_t a, int64_t b, LzValue *result) {
    if (b == 0) {
        return division_by_zero(vm);
    }
    if (b == -1) {
        if (a == INT64_MIN) {
            return lz_vm_set_error(vm, LZ_ERR_RUNTIME, "integer overflow");
        }
        *result = lz_value_int(-a);
        return LZ_OK;
    }

    int64_t quotient = a / b;
    if (a % b != 0 && ((a < 0) != (b < 0))) {
        quotient--;
    }
    *result = lz_value_int(quotient);
    return LZ_OK;
}

static LzStatus integer_modulo(LzVM *vm, int64_t a, int64_t b, LzValue *result) {
    if (b == 0) {
        return division_by_zero(vm);
    }
    if (b == -1) {
        *result = lz_value_int(0);
        return LZ_OK;
    }

    int64_t remainder = a % b;
    if (remainder != 0 && ((remainder < 0) != (b < 0))) {
        remainder += b;
    }
    *result = lz_value_int(remainder);
    return LZ_OK;
}

static void float_divmod(double a, double b, double *floor_div, double *mod) {
    double remainder = fmod(a, b);
    double quotient = (a - remainder) / b;

    if (remainder != 0.0) {
        if ((b < 0.0) != (remainder < 0.0)) {
            remainder += b;
            quotient -= 1.0;
        }
    } else {
        remainder = copysign(0.0, b);
    }

    double floored;
    if (quotient != 0.0) {
        floored = floor(quotient);
        if (quotient - floored > 0.5) {
            floored += 1.0;
        }
    } else {
        floored = copysign(0.0, a / b);
    }

    *floor_div = floored;
    *mod = remainder;
}

static LzStatus integer_arith(LzVM *vm, LzOpCode op, int64_t a, int64_t b, LzValue *result) {
    int64_t out = 0;
    bool overflow;

    switch (op) {
    case LZ_OP_ADD:
        overflow = add_overflows(a, b, &out);
        break;
    case LZ_OP_SUBTRACT:
        overflow = subtract_overflows(a, b, &out);
        break;
    case LZ_OP_MULTIPLY:
        overflow = multiply_overflows(a, b, &out);
        break;
    case LZ_OP_FLOOR_DIVIDE:
        return integer_floor_divide(vm, a, b, result);
    case LZ_OP_MODULO:
        return integer_modulo(vm, a, b, result);
    default:
        return lz_vm_set_error(vm, LZ_ERR_RUNTIME, "unknown arithmetic operator");
    }

    if (overflow) {
        return lz_vm_set_error(vm, LZ_ERR_RUNTIME, "integer overflow");
    }
    *result = lz_value_int(out);
    return LZ_OK;
}

static LzStatus float_arith(LzVM *vm, LzOpCode op, double a, double b, LzValue *result) {
    switch (op) {
    case LZ_OP_ADD:
        *result = lz_value_float(a + b);
        return LZ_OK;
    case LZ_OP_SUBTRACT:
        *result = lz_value_float(a - b);
        return LZ_OK;
    case LZ_OP_MULTIPLY:
        *result = lz_value_float(a * b);
        return LZ_OK;
    case LZ_OP_FLOOR_DIVIDE:
    case LZ_OP_MODULO: {
        if (b == 0.0) {
            return division_by_zero(vm);
        }
        double floor_div;
        double mod;
        float_divmod(a, b, &floor_div, &mod);
        *result = lz_value_float(op == LZ_OP_FLOOR_DIVIDE ? floor_div : mod);
        return LZ_OK;
    }
    default:
        return lz_vm_set_error(vm, LZ_ERR_RUNTIME, "unknown arithmetic operator");
    }
}

static const char *bit_symbol(LzOpCode op) {
    switch (op) {
    case LZ_OP_BIT_AND:
        return "&";
    case LZ_OP_BIT_OR:
        return "|";
    case LZ_OP_BIT_XOR:
        return "^";
    case LZ_OP_BIT_NOT:
        return "~";
    case LZ_OP_SHL:
        return "<<";
    default:
        return ">>";
    }
}

LzStatus lz_arith_bit_type_error(LzVM *vm, LzOpCode op, const LzValue *left, const LzValue *right) {
    const char *symbol = bit_symbol(op);
    if (right == NULL) {
        return lz_vm_set_error(vm, LZ_ERR_RUNTIME, "operator '%s' needs an int, got %s", symbol,
                               lz_kind_name(lz_kind_of(left)));
    }
    const char *hint = "";
    if (left->type == LZ_VAL_BOOL || right->type == LZ_VAL_BOOL) {
        hint = op == LZ_OP_BIT_AND ? " (use 'and' for logic)"
               : op == LZ_OP_BIT_OR ? " (use 'or' for logic)"
                                    : "";
    }
    return lz_vm_set_error(vm, LZ_ERR_RUNTIME, "operator '%s' needs two ints, got %s and %s%s",
                           symbol, lz_kind_name(lz_kind_of(left)), lz_kind_name(lz_kind_of(right)),
                           hint);
}

LzStatus lz_arith_shift_count_error(LzVM *vm, int64_t count) {
    return lz_vm_set_error(vm, LZ_ERR_RUNTIME, "shift count must be 0..63, got %lld",
                           (long long)count);
}

static LzStatus bitwise(LzVM *vm, LzOpCode op, const LzValue *left, const LzValue *right,
                        LzValue *result) {
    if (left->type != LZ_VAL_INT || right->type != LZ_VAL_INT) {
        return lz_arith_bit_type_error(vm, op, left, right);
    }
    int64_t a = left->as.integer;
    int64_t b = right->as.integer;
    switch (op) {
    case LZ_OP_BIT_AND:
        *result = lz_value_int(a & b);
        return LZ_OK;
    case LZ_OP_BIT_OR:
        *result = lz_value_int(a | b);
        return LZ_OK;
    case LZ_OP_BIT_XOR:
        *result = lz_value_int(a ^ b);
        return LZ_OK;
    case LZ_OP_SHL:
    case LZ_OP_SHR:
        if (!lz_shift_count_ok(b)) {
            return lz_arith_shift_count_error(vm, b);
        }
        *result = lz_value_int(op == LZ_OP_SHL ? lz_shift_left(a, b) : lz_shift_right(a, b));
        return LZ_OK;
    default:
        return lz_vm_set_error(vm, LZ_ERR_RUNTIME, "unknown arithmetic operator");
    }
}

LzStatus lz_arith_bit_not(LzVM *vm, const LzValue *operand, LzValue *result) {
    if (operand->type != LZ_VAL_INT) {
        return lz_arith_bit_type_error(vm, LZ_OP_BIT_NOT, operand, NULL);
    }
    *result = lz_value_int(~operand->as.integer);
    return LZ_OK;
}

LzStatus lz_arith_binary(LzVM *vm, LzOpCode op, const LzValue *left, const LzValue *right,
                         LzValue *result) {
    switch (op) {
    case LZ_OP_BIT_AND:
    case LZ_OP_BIT_OR:
    case LZ_OP_BIT_XOR:
    case LZ_OP_SHL:
    case LZ_OP_SHR:
        return bitwise(vm, op, left, right, result);
    default:
        break;
    }
    if (op == LZ_OP_ADD && lz_value_is_array(left) && lz_value_is_array(right)) {
        return lz_array_concat(vm, lz_value_as_array(left), lz_value_as_array(right), result);
    }
    if (op == LZ_OP_ADD && is_string(left) && is_string(right)) {
        return concat(vm, left, right, result);
    }
    if (!is_number(left) || !is_number(right)) {
        return lz_vm_set_error(vm, LZ_ERR_RUNTIME,
                               op == LZ_OP_ADD ? "operands must be two numbers or two strings (or two arrays)"
                                               : "operands must be numbers");
    }

    if (op == LZ_OP_DIVIDE) {
        double divisor = as_double(right);
        if (divisor == 0.0) {
            return division_by_zero(vm);
        }
        *result = lz_value_float(as_double(left) / divisor);
        return LZ_OK;
    }

    if (left->type == LZ_VAL_INT && right->type == LZ_VAL_INT) {
        return integer_arith(vm, op, left->as.integer, right->as.integer, result);
    }
    return float_arith(vm, op, as_double(left), as_double(right), result);
}

LzStatus lz_arith_negate(LzVM *vm, const LzValue *operand, LzValue *result) {
    if (operand->type == LZ_VAL_INT) {
        if (operand->as.integer == INT64_MIN) {
            return lz_vm_set_error(vm, LZ_ERR_RUNTIME, "integer overflow");
        }
        *result = lz_value_int(-operand->as.integer);
        return LZ_OK;
    }
    if (operand->type == LZ_VAL_FLOAT) {
        *result = lz_value_float(-operand->as.number);
        return LZ_OK;
    }
    return lz_vm_set_error(vm, LZ_ERR_RUNTIME, "operand must be a number");
}
