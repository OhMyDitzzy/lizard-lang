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

#endif
