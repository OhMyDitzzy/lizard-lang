#ifndef LIZARD_COMPARE_H
#define LIZARD_COMPARE_H

#include "lizard/chunk.h"
#include "lizard/vm.h"

/*
 * op is LZ_OP_LESS, LZ_OP_GREATER, LZ_OP_LESS_EQUAL or LZ_OP_GREATER_EQUAL. Only numbers are ordered; anything else
 * is a runtime error. Comparisons involving NaN are false. The result is a
 * boolean value.
 */
LzStatus lz_compare_order(LzVM *vm, LzOpCode op, const LzValue *left, const LzValue *right,
                          LzValue *result);

#endif
