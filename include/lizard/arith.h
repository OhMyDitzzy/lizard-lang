#ifndef LIZARD_ARITH_H
#define LIZARD_ARITH_H

#include <stdbool.h>
#include <stdint.h>

#include "lizard/chunk.h"
#include "lizard/vm.h"

/*
 * op is one of LZ_OP_ADD, LZ_OP_SUBTRACT, LZ_OP_MULTIPLY, LZ_OP_DIVIDE, LZ_OP_FLOOR_DIVIDE or
 * LZ_OP_MODULO.
 *
 * Integer operations fail on int64 overflow instead of wrapping, '/' always produces a float, and a
 * zero divisor is an error for '/', '//' and '%'. '//' rounds toward negative infinity and the sign
 * of '%' follows the divisor, as in Python, and with a float operand both give floats. '+' on two
 * strings concatenates, and there's no implicit conversion between strings and numbers. On success
 * *result is a new reference owned by the caller, like a native's result.
 */
LzStatus lz_arith_binary(LzVM *vm, LzOpCode op, const LzValue *left, const LzValue *right,
                         LzValue *result);

LzStatus lz_arith_negate(LzVM *vm, const LzValue *operand, LzValue *result);

/* ~operand, for an int only. */
LzStatus lz_arith_bit_not(LzVM *vm, const LzValue *operand, LzValue *result);

/*
 * Checked int64 arithmetic: each returns true on overflow and otherwise stores the result. The
 * one definition of the overflow rule, shared by lz_arith_binary and the interpreter fast path.
 */
#if defined(__GNUC__) && !defined(LZ_PORTABLE_OVERFLOW)

static inline bool lz_add_overflows(int64_t a, int64_t b, int64_t *out) {
    return __builtin_add_overflow(a, b, out);
}

static inline bool lz_subtract_overflows(int64_t a, int64_t b, int64_t *out) {
    return __builtin_sub_overflow(a, b, out);
}

static inline bool lz_multiply_overflows(int64_t a, int64_t b, int64_t *out) {
    return __builtin_mul_overflow(a, b, out);
}

#else

static inline bool lz_add_overflows(int64_t a, int64_t b, int64_t *out) {
    if ((b > 0 && a > INT64_MAX - b) || (b < 0 && a < INT64_MIN - b)) {
        return true;
    }
    *out = a + b;
    return false;
}

static inline bool lz_subtract_overflows(int64_t a, int64_t b, int64_t *out) {
    if ((b < 0 && a > INT64_MAX + b) || (b > 0 && a < INT64_MIN + b)) {
        return true;
    }
    *out = a - b;
    return false;
}

static inline bool lz_multiply_overflows(int64_t a, int64_t b, int64_t *out) {
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

/*
 * Integer '//' and '%' with the sign rules of the language, shared by lz_arith_binary and the
 * interpreter fast path. False for a zero divisor, and for INT64_MIN // -1, which overflows.
 */
static inline bool lz_int_floor_divide(int64_t a, int64_t b, int64_t *out) {
    if (b == 0) {
        return false;
    }
    if (b == -1) {
        if (a == INT64_MIN) {
            return false;
        }
        *out = -a;
        return true;
    }
    int64_t quotient = a / b;
    if (a % b != 0 && ((a < 0) != (b < 0))) {
        quotient--;
    }
    *out = quotient;
    return true;
}

static inline bool lz_int_modulo(int64_t a, int64_t b, int64_t *out) {
    if (b == 0) {
        return false;
    }
    if (b == -1) {
        *out = 0;
        return true;
    }
    int64_t remainder = a % b;
    if (remainder != 0 && ((remainder < 0) != (b < 0))) {
        remainder += b;
    }
    *out = remainder;
    return true;
}

/*
 * x % y for whole-number floats and a positive divisor, which is what `n / 2` leaves behind. The
 * remainder is exact in integers and equals what fmod plus the sign fix-up gives, without the libm
 * call. False for anything else (fractions, NaN, infinities, a divisor <= 0, magnitudes above
 * 2^53), and the caller then takes the general path.
 */
static inline bool lz_float_modulo_whole(double x, double y, double *out) {
    const double limit = 9007199254740992.0;
    if (y > 0.0 && y <= limit && x >= -limit && x <= limit) {
        int64_t xi = (int64_t)x;
        int64_t yi = (int64_t)y;
        if ((double)xi == x && (double)yi == y) {
            int64_t m = xi % yi;
            if (m < 0) {
                m += yi;
            }
            *out = (double)m;
            return true;
        }
    }
    return false;
}

/*
 * The common case of the six arithmetic operators with nothing to report: two ints that do not
 * overflow and have a nonzero divisor, or two numbers of which one is a float, computed in
 * doubles as lz_arith_binary does (float '//' and '%' only for whole numbers, see above).
 * Returns false and leaves *out alone for anything else, and the caller then takes the general
 * path, which also produces every error message.
 */
static inline bool lz_arith_fast(LzOpCode op, const LzValue *left, const LzValue *right,
                                 LzValue *out) {
    if (left->type == LZ_VAL_INT && right->type == LZ_VAL_INT) {
        int64_t result;
        bool failed;
        switch (op) {
        case LZ_OP_ADD:
            failed = lz_add_overflows(left->as.integer, right->as.integer, &result);
            break;
        case LZ_OP_SUBTRACT:
            failed = lz_subtract_overflows(left->as.integer, right->as.integer, &result);
            break;
        case LZ_OP_MULTIPLY:
            failed = lz_multiply_overflows(left->as.integer, right->as.integer, &result);
            break;
        case LZ_OP_FLOOR_DIVIDE:
            failed = !lz_int_floor_divide(left->as.integer, right->as.integer, &result);
            break;
        case LZ_OP_MODULO:
            failed = !lz_int_modulo(left->as.integer, right->as.integer, &result);
            break;
        case LZ_OP_DIVIDE:
            if (right->as.integer == 0) {
                return false;
            }
            *out = lz_value_float((double)left->as.integer / (double)right->as.integer);
            return true;
        default:
            return false;
        }
        if (failed) {
            return false;
        }
        *out = lz_value_int(result);
        return true;
    }
    if ((left->type == LZ_VAL_FLOAT || left->type == LZ_VAL_INT) &&
        (right->type == LZ_VAL_FLOAT || right->type == LZ_VAL_INT)) {
        double x = left->type == LZ_VAL_INT ? (double)left->as.integer : left->as.number;
        double y = right->type == LZ_VAL_INT ? (double)right->as.integer : right->as.number;
        switch (op) {
        case LZ_OP_ADD:
            *out = lz_value_float(x + y);
            return true;
        case LZ_OP_SUBTRACT:
            *out = lz_value_float(x - y);
            return true;
        case LZ_OP_MULTIPLY:
            *out = lz_value_float(x * y);
            return true;
        case LZ_OP_DIVIDE:
            if (y == 0.0) {
                return false;
            }
            *out = lz_value_float(x / y);
            return true;
        case LZ_OP_MODULO: {
            double r;
            if (!lz_float_modulo_whole(x, y, &r)) {
                return false;
            }
            *out = lz_value_float(r);
            return true;
        }
        default:
            return false;
        }
    }
    return false;
}

/*
 * The shift semantics, shared with the Gecko Engine so every engine agrees. The count must already
 * be checked with lz_shift_count_ok. Both avoid undefined behavior in C, because '<<' works on
 * uint64_t and '>>' never shifts a negative value.
 */
static inline bool lz_shift_count_ok(int64_t count) {
    return count >= 0 && count <= 63;
}

static inline int64_t lz_shift_left(int64_t value, int64_t count) {
    return (int64_t)((uint64_t)value << (unsigned)count);
}

static inline int64_t lz_shift_right(int64_t value, int64_t count) {
    return value >= 0 ? value >> count : ~(~value >> count);
}

/* The error for a shift count outside 0..63. Always returns LZ_ERR_RUNTIME. */
LzStatus lz_arith_shift_count_error(LzVM *vm, int64_t count);

/* The error for a bitwise operand that is not an int. right is NULL for '~'. */
LzStatus lz_arith_bit_type_error(LzVM *vm, LzOpCode op, const LzValue *left, const LzValue *right);

#endif
