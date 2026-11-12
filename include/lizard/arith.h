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
 * The common case of + - * with nothing to report: two ints that do not overflow, or two numbers
 * of which one is a float, which is computed in doubles as lz_arith_binary does. Returns false
 * and leaves *out alone for anything else, and the caller then takes the general path, which also
 * produces every error message.
 */
static inline bool lz_arith_fast(LzOpCode op, const LzValue *left, const LzValue *right,
                                 LzValue *out) {
    if (left->type == LZ_VAL_INT && right->type == LZ_VAL_INT) {
        int64_t result;
        bool overflow;
        switch (op) {
        case LZ_OP_ADD:
            overflow = lz_add_overflows(left->as.integer, right->as.integer, &result);
            break;
        case LZ_OP_SUBTRACT:
            overflow = lz_subtract_overflows(left->as.integer, right->as.integer, &result);
            break;
        case LZ_OP_MULTIPLY:
            overflow = lz_multiply_overflows(left->as.integer, right->as.integer, &result);
            break;
        default:
            return false;
        }
        if (overflow) {
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
