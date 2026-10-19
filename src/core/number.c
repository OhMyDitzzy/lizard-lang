#include "lizard/number.h"

#include <errno.h>
#include <locale.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define MAGNITUDE_LIMIT 9223372036854775808ull

static bool is_digit(char c) {
    return c >= '0' && c <= '9';
}

LzParseResult lz_parse_integer(const char *text, size_t length, int64_t *out) {
    size_t i = 0;
    bool negative = false;
    if (i < length && (text[i] == '+' || text[i] == '-')) {
        negative = text[i] == '-';
        i++;
    }
    if (i >= length) {
        return LZ_PARSE_SYNTAX;
    }
    for (size_t j = i; j < length; j++) {
        if (!is_digit(text[j])) {
            return LZ_PARSE_SYNTAX;
        }
    }

    uint64_t limit = negative ? MAGNITUDE_LIMIT : MAGNITUDE_LIMIT - 1;
    uint64_t value = 0;
    for (; i < length; i++) {
        uint64_t digit = (uint64_t)(text[i] - '0');
        if (value > (limit - digit) / 10) {
            return LZ_PARSE_RANGE;
        }
        value = value * 10 + digit;
    }

    *out = value == MAGNITUDE_LIMIT ? INT64_MIN : (negative ? -(int64_t)value : (int64_t)value);
    return LZ_PARSE_OK;
}

static size_t skip_digits(const char *text, size_t length, size_t i) {
    while (i < length && is_digit(text[i])) {
        i++;
    }
    return i;
}

static bool float_syntax_ok(const char *text, size_t length) {
    size_t i = 0;
    if (i < length && (text[i] == '+' || text[i] == '-')) {
        i++;
    }
    size_t next = skip_digits(text, length, i);
    if (next == i) {
        return false;
    }
    i = next;

    if (i < length && text[i] == '.') {
        next = skip_digits(text, length, i + 1);
        if (next == i + 1) {
            return false;
        }
        i = next;
    }
    if (i < length && (text[i] == 'e' || text[i] == 'E')) {
        i++;
        if (i < length && (text[i] == '+' || text[i] == '-')) {
            i++;
        }
        next = skip_digits(text, length, i);
        if (next == i) {
            return false;
        }
        i = next;
    }
    return i == length;
}

LzParseResult lz_parse_float(const char *text, size_t length, double *out) {
    if (!float_syntax_ok(text, length)) {
        return LZ_PARSE_SYNTAX;
    }

    /* strtod reads the decimal point of the process locale, so swap in that one. */
    const char *point = localeconv()->decimal_point;
    size_t point_length = strlen(point);
    char *buffer = malloc(length + point_length + 1);
    if (buffer == NULL) {
        return LZ_PARSE_NO_MEMORY;
    }

    size_t used = 0;
    for (size_t i = 0; i < length; i++) {
        if (text[i] == '.') {
            memcpy(buffer + used, point, point_length);
            used += point_length;
        } else {
            buffer[used++] = text[i];
        }
    }
    buffer[used] = '\0';

    errno = 0;
    double value = strtod(buffer, NULL);
    bool overflow = errno == ERANGE && isinf(value);
    free(buffer);
    if (overflow) {
        return LZ_PARSE_RANGE;
    }
    *out = value;
    return LZ_PARSE_OK;
}
