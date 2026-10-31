#ifndef LIZARD_FORMAT_H
#define LIZARD_FORMAT_H

#include "lizard/value.h"
#include "lizard/writer.h"

/*
 * The format-spec mini-language of `{x:spec}` and x.format(spec):
 *
 *     [[fill]align] [sign] [#] [0] [width] [group] [.precision] [type]
 *
 *   fill       any one character (default space); align is < left, > right, ^ centre
 *   sign       + always, - only for negatives (default), space
 *   #          0x, 0o or 0b in front of x X o b
 *   0          zeros after the sign (numbers; not with an align)
 *   group      , every 3 digits, or _ every 3 (4 for x o b)
 *   precision  .N digits after the point (f e E %) or characters kept (s)
 *   type       s d x X o b c f e E %
 *
 * The compiler parses a spec written in the source, so a mistake is an error before the
 * program runs; one built at run time is parsed when it is used.
 */

#define LZ_FORMAT_MAX_WIDTH 100000u
#define LZ_FORMAT_MAX_PRECISION 100u

typedef struct LzFormatSpec {
    uint32_t fill;
    char align; /* 0 = the default for the value */
    char sign;  /* 0, '+', '-' or ' ' */
    bool alternate;
    bool zero;
    size_t width;
    char group;
    bool has_precision;
    size_t precision;
    char type; /* 0 = by the value */
} LzFormatSpec;

typedef struct LzFormatError {
    char message[200];
    size_t offset; /* into the spec text */
    size_t length;
    char suggestion; /* a type letter to offer, or 0 */
} LzFormatError;

bool lz_format_parse(const char *text, size_t length, LzFormatSpec *spec, LzFormatError *error);

typedef enum LzFormatResult {
    LZ_FORMAT_OK,
    LZ_FORMAT_BAD,       /* error says why */
    LZ_FORMAT_NO_MEMORY
} LzFormatResult;

/* Writes value formatted by spec. */
LzFormatResult lz_format_value(const LzValue *value, const LzFormatSpec *spec, const LzWriter *out,
                               LzFormatError *error);

#endif
