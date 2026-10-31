#ifndef LIZARD_ARITH_H
#define LIZARD_ARITH_H

#include "lizard/chunk.h"
#include "lizard/vm.h"

/*
 * op is one of LZ_OP_ADD, LZ_OP_SUBTRACT, LZ_OP_MULTIPLY, LZ_OP_DIVIDE,
 * LZ_OP_FLOOR_DIVIDE, LZ_OP_MODULO.
 *
 * Integer operations fail on int64 overflow instead of wrapping, '/' always
 * produces a float, and a zero divisor is an error for '/', '//' and '%'.
 * '//' rounds toward negative infinity and the sign of '%' follows the
 * divisor, as in Python; with a float operand both give floats. '+' on two
 * strings concatenates; there is no implicit conversion between strings and
 * numbers. On success *result is a new reference owned by the caller, like a
 * native's result.
 */
LzStatus lz_arith_binary(LzVM *vm, LzOpCode op, const LzValue *left, const LzValue *right,
                         LzValue *result);

LzStatus lz_arith_negate(LzVM *vm, const LzValue *operand, LzValue *result);

/* ~operand, for an int only. */
LzStatus lz_arith_bit_not(LzVM *vm, const LzValue *operand, LzValue *result);

/*
 * The shift semantics, shared with the Gecko Engine so every engine agrees. The
 * count must already be checked with lz_shift_count_ok. Both avoid undefined
 * behavior in C: '<<' works on uint64_t and '>>' never shifts a negative value.
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

/* The error for a shift count outside 0..63; always returns LZ_ERR_RUNTIME. */
LzStatus lz_arith_shift_count_error(LzVM *vm, int64_t count);

/* The error for a bitwise operand that is not an int (right is NULL for '~'). */
LzStatus lz_arith_bit_type_error(LzVM *vm, LzOpCode op, const LzValue *left, const LzValue *right);

#endif
