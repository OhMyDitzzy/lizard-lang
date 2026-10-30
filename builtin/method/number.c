#include <math.h>
#include <stdint.h>

#include "method/method.h"

/* Number methods. An int stays an int where the answer is whole; sqrt and fractional powers give floats. */

static bool is_number(const LzValue *value) {
    return value->type == LZ_VAL_INT || value->type == LZ_VAL_FLOAT;
}

static double as_double(const LzValue *value) {
    return value->type == LZ_VAL_INT ? (double)value->as.integer : value->as.number;
}

static LzStatus want_number(LzVM *vm, const char *name, const LzValue *value) {
    if (!is_number(value)) {
        return lz_vm_set_error(vm, LZ_ERR_RUNTIME, "%s(): expected a number, got %s", name,
                               lz_kind_name(lz_kind_of(value)));
    }
    return LZ_OK;
}

/* A whole float as an int, or an error the same way int() reports it. */
static LzStatus to_int_value(LzVM *vm, const char *name, double number, LzValue *result) {
    if (isnan(number) || isinf(number)) {
        return lz_vm_set_error(vm, LZ_ERR_RUNTIME, "%s(): cannot convert %s to an int", name,
                               isnan(number) ? "nan" : "infinity");
    }
    if (number >= 9223372036854775808.0 || number < -9223372036854775808.0) {
        return lz_vm_set_error(vm, LZ_ERR_RUNTIME, "%s(): result out of range for int", name);
    }
    *result = lz_value_int((int64_t)number);
    return LZ_OK;
}

LzStatus lz_method_abs(LzVM *vm, int argc, const LzValue *argv, LzValue *result) {
    (void)argc;
    LzStatus status = want_number(vm, "abs", &argv[0]);
    if (status != LZ_OK) {
        return status;
    }
    if (argv[0].type == LZ_VAL_FLOAT) {
        *result = lz_value_float(fabs(argv[0].as.number));
    } else if (argv[0].as.integer == INT64_MIN) {
        return lz_vm_set_error(vm, LZ_ERR_RUNTIME, "abs(): integer overflow");
    } else {
        *result = lz_value_int(argv[0].as.integer < 0 ? -argv[0].as.integer : argv[0].as.integer);
    }
    return LZ_OK;
}

typedef double (*Rounding)(double);

static LzStatus rounded(LzVM *vm, const char *name, const LzValue *argv, Rounding round_with,
                        LzValue *result) {
    LzStatus status = want_number(vm, name, &argv[0]);
    if (status != LZ_OK) {
        return status;
    }
    if (argv[0].type == LZ_VAL_INT) {
        *result = argv[0];
        return LZ_OK;
    }
    return to_int_value(vm, name, round_with(argv[0].as.number), result);
}

LzStatus lz_method_floor(LzVM *vm, int argc, const LzValue *argv, LzValue *result) {
    (void)argc;
    return rounded(vm, "floor", argv, floor, result);
}

LzStatus lz_method_ceil(LzVM *vm, int argc, const LzValue *argv, LzValue *result) {
    (void)argc;
    return rounded(vm, "ceil", argv, ceil, result);
}

/* Halves round away from zero: 2.5 -> 3, -2.5 -> -3. */
LzStatus lz_method_round(LzVM *vm, int argc, const LzValue *argv, LzValue *result) {
    (void)argc;
    return rounded(vm, "round", argv, round, result);
}

LzStatus lz_method_sqrt(LzVM *vm, int argc, const LzValue *argv, LzValue *result) {
    (void)argc;
    LzStatus status = want_number(vm, "sqrt", &argv[0]);
    if (status != LZ_OK) {
        return status;
    }
    double number = as_double(&argv[0]);
    if (number < 0) {
        return lz_vm_set_error(vm, LZ_ERR_RUNTIME, "sqrt(): cannot take the square root of a negative number");
    }
    *result = lz_value_float(sqrt(number));
    return LZ_OK;
}

/* a * b, false on overflow. */
static bool multiply(int64_t a, int64_t b, int64_t *out) {
    if (a == 0 || b == 0) {
        *out = 0;
        return true;
    }
    if ((a == -1 && b == INT64_MIN) || (b == -1 && a == INT64_MIN)) {
        return false;
    }
    if (a > 0 ? (b > 0 ? a > INT64_MAX / b : b < INT64_MIN / a)
              : (b > 0 ? a < INT64_MIN / b : a < INT64_MAX / b)) {
        return false;
    }
    *out = a * b;
    return true;
}

LzStatus lz_method_pow(LzVM *vm, int argc, const LzValue *argv, LzValue *result) {
    (void)argc;
    LzStatus status = want_number(vm, "pow", &argv[0]);
    if (status == LZ_OK) {
        status = want_number(vm, "pow", &argv[1]);
    }
    if (status != LZ_OK) {
        return status;
    }
    if (argv[0].type == LZ_VAL_INT && argv[1].type == LZ_VAL_INT && argv[1].as.integer >= 0) {
        int64_t base = argv[0].as.integer;
        int64_t exponent = argv[1].as.integer;
        int64_t power = 1;
        while (exponent > 0) {
            if ((exponent & 1) != 0 && !multiply(power, base, &power)) {
                return lz_vm_set_error(vm, LZ_ERR_RUNTIME, "pow(): integer overflow");
            }
            exponent >>= 1;
            if (exponent > 0 && !multiply(base, base, &base)) {
                return lz_vm_set_error(vm, LZ_ERR_RUNTIME, "pow(): integer overflow");
            }
        }
        *result = lz_value_int(power);
        return LZ_OK;
    }
    double value = pow(as_double(&argv[0]), as_double(&argv[1]));
    if (isnan(value)) {
        return lz_vm_set_error(vm, LZ_ERR_RUNTIME, "pow(): the result is not a number");
    }
    *result = lz_value_float(value);
    return LZ_OK;
}

static LzStatus extreme(LzVM *vm, const char *name, const LzValue *argv, bool want_max,
                        LzValue *result) {
    LzStatus status = want_number(vm, name, &argv[0]);
    if (status == LZ_OK) {
        status = want_number(vm, name, &argv[1]);
    }
    if (status != LZ_OK) {
        return status;
    }
    double a = as_double(&argv[0]);
    double b = as_double(&argv[1]);
    if (isnan(a) || isnan(b)) {
        return lz_vm_set_error(vm, LZ_ERR_RUNTIME, "%s(): cannot compare nan", name);
    }
    /* The winner keeps its own type; on a tie the receiver wins. */
    *result = (want_max ? b > a : b < a) ? argv[1] : argv[0];
    return LZ_OK;
}

LzStatus lz_method_min(LzVM *vm, int argc, const LzValue *argv, LzValue *result) {
    (void)argc;
    return extreme(vm, "min", argv, false, result);
}

LzStatus lz_method_max(LzVM *vm, int argc, const LzValue *argv, LzValue *result) {
    (void)argc;
    return extreme(vm, "max", argv, true, result);
}
