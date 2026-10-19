#ifndef LIZARD_NUMBER_H
#define LIZARD_NUMBER_H

#include "lizard/common.h"

typedef enum LzParseResult {
    LZ_PARSE_OK,
    LZ_PARSE_SYNTAX,
    LZ_PARSE_RANGE,
    LZ_PARSE_NO_MEMORY
} LzParseResult;

/* [+-]digits, base 10, no spaces. Accepts exactly the int64 range. */
LzParseResult lz_parse_integer(const char *text, size_t length, int64_t *out);

/*
 * [+-]digits[.digits][(e|E)[+-]digits], the same shape as a float literal, no
 * spaces, no inf or nan. Independent of the process locale. Fails with
 * LZ_PARSE_RANGE when the value overflows to infinity.
 */
LzParseResult lz_parse_float(const char *text, size_t length, double *out);

#endif
