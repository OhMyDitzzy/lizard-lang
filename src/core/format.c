#include "lizard/format.h"
#include "lizard/type.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TYPES "sdxXobcfeE%"

static size_t cp_length(const char *text, size_t length, size_t at) {
    unsigned char c = (unsigned char)text[at];
    size_t want = c < 0x80 ? 1 : c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
    if (at + want > length) {
        return 1;
    }
    for (size_t i = 1; i < want; i++) {
        if (((unsigned char)text[at + i] & 0xC0) != 0x80) {
            return 1;
        }
    }
    return want;
}

static uint32_t cp_decode(const char *text, size_t at, size_t len) {
    unsigned char c = (unsigned char)text[at];
    if (len == 1) {
        return c;
    }
    uint32_t value = len == 2 ? (c & 0x1Fu) : len == 3 ? (c & 0x0Fu) : (c & 0x07u);
    for (size_t i = 1; i < len; i++) {
        value = (value << 6) | ((unsigned char)text[at + i] & 0x3Fu);
    }
    return value;
}

static size_t cp_count(const char *text, size_t length) {
    size_t count = 0;
    for (size_t i = 0; i < length; i++) {
        if (((unsigned char)text[i] & 0xC0) != 0x80) {
            count++;
        }
    }
    return count;
}

static size_t encode(uint32_t scalar, char *bytes) {
    if (scalar < 0x80) {
        bytes[0] = (char)scalar;
        return 1;
    }
    if (scalar < 0x800) {
        bytes[0] = (char)(0xC0 | (scalar >> 6));
        bytes[1] = (char)(0x80 | (scalar & 0x3F));
        return 2;
    }
    if (scalar < 0x10000) {
        bytes[0] = (char)(0xE0 | (scalar >> 12));
        bytes[1] = (char)(0x80 | ((scalar >> 6) & 0x3F));
        bytes[2] = (char)(0x80 | (scalar & 0x3F));
        return 3;
    }
    bytes[0] = (char)(0xF0 | (scalar >> 18));
    bytes[1] = (char)(0x80 | ((scalar >> 12) & 0x3F));
    bytes[2] = (char)(0x80 | ((scalar >> 6) & 0x3F));
    bytes[3] = (char)(0x80 | (scalar & 0x3F));
    return 4;
}

static bool is_align(char c) {
    return c == '<' || c == '>' || c == '^';
}

static bool is_digit(char c) {
    return c >= '0' && c <= '9';
}

static void bad(LzFormatError *error, size_t offset, size_t length, const char *format, ...)
    LZ_PRINTF_FORMAT(4, 5);

static void bad(LzFormatError *error, size_t offset, size_t length, const char *format, ...) {
    va_list args;
    va_start(args, format);
    (void)vsnprintf(error->message, sizeof error->message, format, args);
    va_end(args);
    error->offset = offset;
    error->length = length == 0 ? 1 : length;
}

static bool read_number(const char *t, size_t n, size_t *p, size_t limit, const char *what,
                        size_t *out, LzFormatError *error) {
    size_t start = *p;
    size_t value = 0;
    while (*p < n && is_digit(t[*p])) {
        value = value * 10 + (size_t)(t[*p] - '0');
        if (value > limit) {
            while (*p < n && is_digit(t[*p])) {
                (*p)++;
            }
            bad(error, start, *p - start, "%s is too large (the limit is %zu)", what, limit);
            return false;
        }
        (*p)++;
    }
    *out = value;
    return true;
}

bool lz_format_parse(const char *t, size_t n, LzFormatSpec *s, LzFormatError *error) {
    memset(s, 0, sizeof *s);
    memset(error, 0, sizeof *error);
    s->fill = ' ';
    size_t p = 0;

    if (n > 0) {
        size_t first = cp_length(t, n, 0);
        if (first < n && is_align(t[first])) {
            s->fill = cp_decode(t, 0, first);
            s->align = t[first];
            p = first + 1;
        } else if (is_align(t[0])) {
            s->align = t[0];
            p = 1;
        }
    }
    if (p < n && (t[p] == '+' || t[p] == '-' || t[p] == ' ')) {
        s->sign = t[p++];
    }
    if (p < n && t[p] == '#') {
        s->alternate = true;
        p++;
    }
    size_t zero_at = p;
    if (p < n && t[p] == '0') {
        s->zero = true;
        p++;
    }
    if (!read_number(t, n, &p, LZ_FORMAT_MAX_WIDTH, "the width", &s->width, error)) {
        return false;
    }
    if (p < n && (t[p] == ',' || t[p] == '_')) {
        s->group = t[p++];
    }
    if (p < n && t[p] == '.') {
        size_t dot = p++;
        if (p >= n || !is_digit(t[p])) {
            bad(error, dot, 1, "expected the number of digits after '.' in the format spec");
            return false;
        }
        s->has_precision = true;
        if (!read_number(t, n, &p, LZ_FORMAT_MAX_PRECISION, "the precision", &s->precision, error)) {
            return false;
        }
    }
    if (p < n) {
        size_t len = cp_length(t, n, p);
        bool single = p + len == n;
        if (!single || len != 1 || strchr(TYPES, t[p]) == NULL) {
            size_t rest = n - p;
            if (single && len == 1 && t[p] != '\0') {
                char lower = (char)((t[p] >= 'A' && t[p] <= 'Z') ? t[p] + 32 : t[p]);
                bad(error, p, len, "unknown format type '%c' (use one of s d x X o b c f e E %%)", t[p]);
                if (lower != t[p] && strchr(TYPES, lower) != NULL) {
                    error->suggestion = lower;
                }
            } else {
                bad(error, p, rest, "unexpected '%.*s' in the format spec", (int)(rest < 20 ? rest : 20),
                    t + p);
            }
            return false;
        }
        s->type = t[p];
    }

    char type = s->type;
    if (s->zero && s->align != 0) {
        bad(error, zero_at, 1, "'0' cannot be combined with an alignment (< > ^)");
        return false;
    }
    if (s->alternate && type != 0 && strchr("xXob", type) == NULL) {
        bad(error, 0, n, "'#' only works with the types x, X, o and b");
        return false;
    }
    if (s->group == ',' && type != 0 && strchr("xXobcs", type) != NULL) {
        bad(error, 0, n, "',' groups decimal digits; use '_' with x, X, o and b");
        return false;
    }
    if (s->group != 0 && type != 0 && strchr("cs", type) != NULL) {
        bad(error, 0, n, "a digit group does not apply to type '%c'", type);
        return false;
    }
    if (s->has_precision && type != 0 && strchr("dxXobc", type) != NULL) {
        bad(error, 0, n, "a precision does not apply to type '%c' (it works with f, e, E, %% and s)",
            type);
        return false;
    }
    if (s->sign != 0 && type != 0 && strchr("sc", type) != NULL) {
        bad(error, 0, n, "a sign does not apply to type '%c'", type);
        return false;
    }
    if (s->zero && type != 0 && strchr("sc", type) != NULL) {
        bad(error, zero_at, 1, "'0' only works with numbers, not type '%c'", type);
        return false;
    }
    return true;
}

/* ---- applying a spec ---- */

typedef struct Out {
    const LzWriter *writer;
    bool ok;
} Out;

static void put(Out *out, const char *data, size_t length) {
    if (out->ok && length > 0) {
        out->ok = lz_writer_write(out->writer, data, length);
    }
}

static void put_fill(Out *out, uint32_t fill, size_t count) {
    char bytes[4];
    size_t size = encode(fill, bytes);
    for (size_t i = 0; i < count; i++) {
        put(out, bytes, size);
    }
}

static void put_repeat(Out *out, char c, size_t count) {
    while (count > 0 && out->ok) {
        char chunk[64];
        size_t n = count < sizeof chunk ? count : sizeof chunk;
        memset(chunk, c, n);
        put(out, chunk, n);
        count -= n;
    }
}

/* Pads text (already built) to the width. */
static void put_aligned(Out *out, const LzFormatSpec *s, char default_align, const char *text,
                        size_t length) {
    size_t chars = cp_count(text, length);
    size_t pad = s->width > chars ? s->width - chars : 0;
    char align = s->align != 0 ? s->align : default_align;
    size_t left = align == '>' ? pad : align == '^' ? pad / 2 : 0;
    put_fill(out, s->fill, left);
    put(out, text, length);
    put_fill(out, s->fill, pad - left);
}

static bool kind_is_number(const LzValue *v) {
    return v->type == LZ_VAL_INT || v->type == LZ_VAL_FLOAT;
}

static LzFormatResult text_result(Out *out) {
    return out->ok ? LZ_FORMAT_OK : LZ_FORMAT_NO_MEMORY;
}

/* Digits of m in base, most significant first. Returns the count. */
static size_t digits_of(uint64_t m, unsigned base, bool upper, char *buffer) {
    char reversed[72];
    size_t count = 0;
    const char *alphabet = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    do {
        reversed[count++] = alphabet[m % base];
        m /= base;
    } while (m > 0);
    for (size_t i = 0; i < count; i++) {
        buffer[i] = reversed[count - 1 - i];
    }
    return count;
}

static size_t grouped_length(size_t digits, size_t every) {
    return digits + (digits > 0 ? (digits - 1) / every : 0);
}

/* digits with `zeros` zeros in front and a separator every `every` digits. */
static void put_grouped(Out *out, const char *digits, size_t count, size_t zeros, char sep,
                        size_t every) {
    size_t total = count + zeros;
    for (size_t i = 0; i < total; i++) {
        if (i > 0 && (total - i) % every == 0) {
            put(out, &sep, 1);
        }
        put(out, i < zeros ? "0" : digits + (i - zeros), 1);
    }
}

typedef struct Number {
    char sign[2];       /* "", "-", "+" or " " */
    char prefix[3];
    const char *digits; /* integer digits */
    size_t digit_count;
    const char *rest;   /* ".25", "e+03" ... */
    size_t rest_length;
    const char *suffix; /* "%" */
    bool special;       /* nan or inf: no zeros, no groups */
} Number;

static void put_number(Out *out, const LzFormatSpec *s, const Number *n) {
    size_t every = s->group == '_' && s->type != 0 && strchr("xXob", s->type) != NULL ? 4 : 3;
    size_t sign = strlen(n->sign);
    size_t prefix = strlen(n->prefix);
    size_t suffix = strlen(n->suffix);
    bool grouping = s->group != 0 && !n->special;

    size_t fixed = sign + prefix + n->rest_length + suffix;
    size_t body = grouping ? grouped_length(n->digit_count, every) : n->digit_count;
    size_t zeros = 0;
    size_t pad = 0;

    if (s->zero && !n->special) {
        if (grouping) {
            while (fixed + grouped_length(n->digit_count + zeros, every) < s->width) {
                zeros++;
            }
        } else if (fixed + body < s->width) {
            zeros = s->width - fixed - body;
        }
    } else if (fixed + body < s->width) {
        pad = s->width - fixed - body;
    }

    char align = s->align != 0 ? s->align : '>';
    size_t left = align == '>' ? pad : align == '^' ? pad / 2 : 0;
    put_fill(out, s->fill, left);
    put(out, n->sign, sign);
    put(out, n->prefix, prefix);
    if (grouping) {
        put_grouped(out, n->digits, n->digit_count, zeros, s->group, every);
    } else {
        put_repeat(out, '0', zeros);
        put(out, n->digits, n->digit_count);
    }
    put(out, n->rest, n->rest_length);
    put(out, n->suffix, suffix);
    put_fill(out, s->fill, pad - left);
}

static void set_sign(Number *n, const LzFormatSpec *s, bool negative) {
    n->sign[0] = negative ? '-' : (s->sign == '+' || s->sign == ' ') ? s->sign : '\0';
    n->sign[1] = '\0';
}

static LzFormatResult format_text(const LzValue *v, const LzFormatSpec *s, const LzWriter *writer,
                                  LzFormatError *error, bool as_debug) {
    (void)as_debug;
    Out out = {writer, true};

    if (s->sign != 0 || s->alternate || s->zero || s->group != 0) {
        bad(error, 0, 1, "%s only works with numbers, but the value is %s",
            s->sign != 0 ? "a sign" : s->alternate ? "'#'" : s->zero ? "'0'" : "a digit group",
            lz_kind_name(lz_kind_of(v)));
        return LZ_FORMAT_BAD;
    }

    LzBuffer text = {0};
    LzWriter into = lz_writer_from_buffer(&text);
    if (!lz_value_write(v, &into)) {
        lz_buffer_free(&text);
        return LZ_FORMAT_NO_MEMORY;
    }
    size_t length = text.length;
    if (s->has_precision) {
        size_t kept = 0;
        size_t i = 0;
        while (i < length && kept < s->precision) {
            i += cp_length(text.data, length, i);
            kept++;
        }
        length = i;
    }
    put_aligned(&out, s, '<', text.data != NULL ? text.data : "", length);
    lz_buffer_free(&text);
    return text_result(&out);
}

LzFormatResult lz_format_value(const LzValue *v, const LzFormatSpec *s, const LzWriter *writer,
                               LzFormatError *error) {
    memset(error, 0, sizeof *error);
    Out out = {writer, true};
    char type = s->type;
    bool is_int = v->type == LZ_VAL_INT;
    bool is_float = v->type == LZ_VAL_FLOAT;

    if (type == 0) {
        if (is_int) {
            type = 'd';
            if (s->has_precision) {
                bad(error, 0, 1, "a precision does not apply to an int (use a float, or type 'f')");
                return LZ_FORMAT_BAD;
            }
            if (s->alternate) {
                bad(error, 0, 1, "'#' needs type x, X, o or b");
                return LZ_FORMAT_BAD;
            }
        } else if (is_float) {
            type = s->has_precision ? 'f' : 'g';
            if (s->alternate) {
                bad(error, 0, 1, "'#' needs type x, X, o or b");
                return LZ_FORMAT_BAD;
            }
        } else {
            return format_text(v, s, writer, error, false);
        }
    }
    if (type == 's') {
        return format_text(v, s, writer, error, false);
    }

    if (type == 'c') {
        if (!is_int) {
            bad(error, 0, 1, "format type 'c' needs an int (a code point), but the value is %s",
                lz_kind_name(lz_kind_of(v)));
            return LZ_FORMAT_BAD;
        }
        int64_t scalar = v->as.integer;
        if (scalar < 0 || scalar > 0x10FFFF || (scalar >= 0xD800 && scalar <= 0xDFFF)) {
            bad(error, 0, 1, "format type 'c': %lld is not a valid code point", (long long)scalar);
            return LZ_FORMAT_BAD;
        }
        char bytes[4];
        size_t size = encode((uint32_t)scalar, bytes);
        put_aligned(&out, s, '<', bytes, size);
        return text_result(&out);
    }

    if (strchr("dxXob", type) != NULL) {
        if (!is_int) {
            if (is_float) {
                bad(error, 0, 1,
                    "format type '%c' needs an int, but the value is a float (use 'f', or round "
                    "it first with .round())",
                    type);
            } else {
                bad(error, 0, 1, "format type '%c' needs an int, but the value is %s", type,
                    lz_kind_name(lz_kind_of(v)));
            }
            return LZ_FORMAT_BAD;
        }
        int64_t value = v->as.integer;
        uint64_t magnitude = value < 0 ? (uint64_t)0 - (uint64_t)value : (uint64_t)value;
        unsigned base = type == 'd' ? 10 : (type == 'x' || type == 'X') ? 16 : type == 'o' ? 8 : 2;
        char digits[72];
        Number n = {0};
        n.digit_count = digits_of(magnitude, base, type == 'X', digits);
        n.digits = digits;
        n.rest = "";
        n.suffix = "";
        set_sign(&n, s, value < 0);
        if (s->alternate) {
            n.prefix[0] = '0';
            n.prefix[1] = type == 'X' ? 'X' : type;
        }
        put_number(&out, s, &n);
        return text_result(&out);
    }

    /* f e E % g */
    if (!kind_is_number(v)) {
        bad(error, 0, 1, "format type '%c' needs a number, but the value is %s", type,
            lz_kind_name(lz_kind_of(v)));
        return LZ_FORMAT_BAD;
    }
    double number = is_int ? (double)v->as.integer : v->as.number;
    Number n = {0};
    n.prefix[0] = '\0';
    n.suffix = "";
    n.rest = "";

    if (isnan(number) || isinf(number)) {
        n.special = true;
        n.digits = isnan(number) ? "nan" : "inf";
        n.digit_count = 3;
        set_sign(&n, s, isinf(number) && number < 0);
        put_number(&out, s, &n);
        return text_result(&out);
    }

    char text[512];
    size_t precision = s->has_precision ? s->precision : 6;
    int written;
    if (type == 'g') {
        LzBuffer shortest = {0};
        LzWriter into = lz_writer_from_buffer(&shortest);
        if (!lz_value_write(v, &into) || shortest.length >= sizeof text) {
            lz_buffer_free(&shortest);
            return LZ_FORMAT_NO_MEMORY;
        }
        memcpy(text, shortest.data, shortest.length);
        written = (int)shortest.length;
        lz_buffer_free(&shortest);
    } else if (type == 'e' || type == 'E') {
        written = snprintf(text, sizeof text, type == 'e' ? "%.*e" : "%.*E", (int)precision, number);
    } else {
        double shown = type == '%' ? number * 100.0 : number;
        written = snprintf(text, sizeof text, "%.*f", (int)precision, shown);
        if (type == '%') {
            n.suffix = "%";
        }
    }
    if (written < 0 || (size_t)written >= sizeof text) {
        bad(error, 0, 1, "the number is too large to format with this precision");
        return LZ_FORMAT_BAD;
    }

    const char *body = text;
    bool negative = false;
    if (body[0] == '-') {
        negative = true;
        body++;
        written--;
    }
    size_t int_digits = 0;
    while (int_digits < (size_t)written && is_digit(body[int_digits])) {
        int_digits++;
    }
    set_sign(&n, s, negative);
    n.digits = body;
    n.digit_count = int_digits;
    n.rest = body + int_digits;
    n.rest_length = (size_t)written - int_digits;
    put_number(&out, s, &n);
    return text_result(&out);
}
