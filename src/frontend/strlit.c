#include "lizard/strlit.h"

#include <string.h>

#define MAX_STRING_NESTING 16u

static bool is_quote(char c) {
    return c == '"' || c == '\'';
}

static bool is_alpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static bool is_digit(char c) {
    return c >= '0' && c <= '9';
}

static bool is_blank(char c) {
    return c == ' ' || c == '\t';
}

static int hex_value(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

static size_t line_end_of(const char *text, size_t length, size_t pos) {
    while (pos < length && text[pos] != '\n') {
        pos++;
    }
    return pos;
}

static size_t quote_run(const char *text, size_t length, size_t pos) {
    size_t run = 0;
    while (pos + run < length && text[pos + run] == text[pos]) {
        run++;
    }
    return run;
}

/* resume == SIZE_MAX: the caller of lz_string_scan decides. */
static bool fail(LzStringError *error, const char *message, size_t offset, size_t length) {
    error->message = message;
    error->offset = offset;
    error->length = length;
    error->resume = SIZE_MAX;
    return false;
}

typedef struct Scan {
    const char *text;
    size_t length;
} Scan;

static bool scan_string(const Scan *s, size_t start, unsigned depth, LzStringHead *head,
                        LzStringError *error);

/*
 * From just after a '{': finds where the expression ends, the optional format spec and the
 * closing '}'. Strings inside are skipped whole, brackets are matched, and the first ':'
 * outside every bracket starts the spec.
 */
static bool scan_braces(const Scan *s, size_t open, unsigned depth, bool multiline,
                        LzStringBraces *out, LzStringError *error) {
    const char *text = s->text;
    size_t length = s->length;
    size_t p = open + 1;
    unsigned nest = 0;
    memset(out, 0, sizeof *out);

    for (;;) {
        if (p >= length) {
            return fail(error, "unclosed '{' in a template string (write '{{' for a literal brace)",
                        open, 1);
        }
        char c = text[p];
        if (c == '\n') {
            if (!multiline) {
                return fail(error,
                            "unclosed '{' in a template string (a single-line string cannot "
                            "continue on the next line)",
                            open, 1);
            }
            p++;
            continue;
        }
        if (c == '#') {
            return fail(error, "a comment cannot appear inside '{}' of a template string", p, 1);
        }
        if (is_quote(c) || c == '$') {
            LzStringHead inner;
            if (!scan_string(s, p, depth + 1, &inner, error)) {
                if (strncmp(error->message, "unterminated", 12) == 0) {
                    return fail(error,
                                "unclosed '{' in a template string (write '{{' for a literal brace)",
                                open, 1);
                }
                return false;
            }
            p = inner.end;
            continue;
        }
        if (is_alpha(c)) {
            size_t q = p;
            while (q < length && (is_alpha(text[q]) || is_digit(text[q]))) {
                q++;
            }
            if (q - p == 1 && c == 'r' && q < length && is_quote(text[q])) {
                LzStringHead inner;
                if (!scan_string(s, p, depth + 1, &inner, error)) {
                    return false;
                }
                p = inner.end;
            } else {
                p = q;
            }
            continue;
        }
        if (is_digit(c)) {
            while (p < length && (is_alpha(text[p]) || is_digit(text[p]))) {
                p++;
            }
            continue;
        }
        if (c == '(' || c == '[' || c == '{') {
            nest++;
        } else if (c == ')' || c == ']') {
            if (nest > 0) {
                nest--;
            }
        } else if (c == '}') {
            if (nest == 0) {
                out->expr_end = p;
                out->close = p;
                return true;
            }
            nest--;
        } else if (c == ':' && nest == 0) {
            out->expr_end = p;
            out->has_spec = true;
            out->spec_start = p + 1;
            size_t q = p + 1;
            for (;;) {
                if (q >= length) {
                    return fail(error, "unclosed '{' in a template string", open, 1);
                }
                if (text[q] == '\n') {
                    return fail(error, "a format spec cannot contain a line break", q, 1);
                }
                if (text[q] == '{') {
                    LzStringBraces inner;
                    if (!scan_braces(s, q, depth, multiline, &inner, error)) {
                        return false;
                    }
                    if (inner.has_spec) {
                        return fail(error,
                                    "a width or precision written as {..} cannot have its own spec",
                                    q, inner.close - q + 1);
                    }
                    q = inner.close + 1;
                    continue;
                }
                if (text[q] == '}') {
                    out->spec_end = q;
                    out->close = q;
                    return true;
                }
                q++;
            }
        }
        p++;
    }
}

static bool scan_string(const Scan *s, size_t start, unsigned depth, LzStringHead *head,
                        LzStringError *error) {
    const char *text = s->text;
    size_t length = s->length;
    memset(head, 0, sizeof *head);

    if (depth > MAX_STRING_NESTING) {
        return fail(error, "strings are nested too deeply", start, 1);
    }

    size_t p = start;
    if (text[p] == '$') {
        head->is_template = true;
        p++;
    }
    if (p < length && text[p] == 'r') {
        head->is_raw = true;
        p++;
    }
    if (p >= length || !is_quote(text[p])) {
        fail(error, "'$' must come directly before the opening quote, as in $\"...\"", start, 1);
        error->resume = start + 1;
        return false;
    }

    head->quote = text[p];
    size_t quotes = quote_run(text, length, p);
    size_t open_length = (p - start) + (quotes == 2 ? 1 : quotes);

    if (quotes == 2) { /* "" */
        head->fence = 1;
        head->body_start = head->body_end = p + 1;
        head->end = p + 2;
        head->needs_check = false;
        return true;
    }

    head->fence = quotes;
    p += quotes;
    head->body_start = p;
    head->needs_check = head->is_template;

    if (quotes >= 3) {
        size_t q = p;
        while (q < length && (is_blank(text[q]) || text[q] == '\r')) {
            q++;
        }
        if (q >= length || text[q] == '\n') {
            head->is_multiline = true;
            head->needs_check = true;
            p = q < length ? q + 1 : q;
            head->body_start = p;
        }
    }

    const char quote = head->quote;
    const size_t fence = head->fence;

    if (!head->is_multiline) {
        for (;;) {
            if (p >= length || text[p] == '\n') {
                fail(error, "unterminated string", start, open_length);
                error->resume = p;
                return false;
            }
            char c = text[p];
            if (c == quote) {
                size_t run = quote_run(text, length, p);
                if (fence == 1 || run == fence) {
                    head->body_end = p;
                    head->end = p + (fence == 1 ? 1 : run);
                    return true;
                }
                if (run > fence) {
                    return fail(error,
                                "ambiguous quotes: this run is longer than the fence that opened "
                                "the string; use a longer fence",
                                p, run);
                }
                p += run;
                continue;
            }
            if (c == '\\' && !head->is_raw) {
                head->needs_check = true;
                p++;
                if (p < length && text[p] != '\n') {
                    p++;
                }
                continue;
            }
            if (head->is_template && c == '{') {
                if (p + 1 < length && text[p + 1] == '{') {
                    p += 2;
                    continue;
                }
                LzStringBraces braces;
                if (!scan_braces(s, p, depth, false, &braces, error)) {
                    return false;
                }
                p = braces.close + 1;
                continue;
            }
            p++;
        }
    }

    /* Multi-line: p is the start of the first line of the body. */
    size_t line = p;
    bool at_line_start = true;
    for (;;) {
        if (at_line_start) {
            at_line_start = false;
            size_t q = line;
            while (q < length && is_blank(text[q])) {
                q++;
            }
            if (q < length && text[q] == quote) {
                size_t run = quote_run(text, length, q);
                if (run == fence) {
                    head->indent_start = line;
                    head->indent_length = q - line;
                    size_t end = line > head->body_start ? line - 1 : head->body_start;
                    if (end > head->body_start && text[end - 1] == '\r') {
                        end--;
                    }
                    head->body_end = end;
                    head->end = q + run;
                    return true;
                }
                if (run > fence) {
                    return fail(error,
                                "ambiguous quotes: this line holds more quotes than the fence "
                                "that opened the string; use a longer fence",
                                q, run);
                }
            }
            p = line;
        }
        if (p >= length) {
            fail(error,
                 "unterminated multi-line string (the closing quotes must be alone on their line)",
                 start, open_length);
            error->resume = length;
            return false;
        }
        char c = text[p];
        if (c == '\n') {
            p++;
            line = p;
            at_line_start = true;
            continue;
        }
        if (c == '\\' && !head->is_raw) {
            p++;
            if (p < length && text[p] != '\n' && text[p] != '\r') {
                p++;
            }
            continue;
        }
        if (head->is_template && c == '{') {
            if (p + 1 < length && text[p + 1] == '{') {
                p += 2;
                continue;
            }
            LzStringBraces braces;
            if (!scan_braces(s, p, depth, true, &braces, error)) {
                return false;
            }
            p = braces.close + 1;
            continue;
        }
        p++;
    }
}

bool lz_string_starts_at(const LzSource *source, size_t at) {
    if (at >= source->length) {
        return false;
    }
    char c = source->text[at];
    if (is_quote(c) || c == '$') {
        return true;
    }
    return c == 'r' && at + 1 < source->length && is_quote(source->text[at + 1]);
}

bool lz_string_scan(const LzSource *source, size_t start, LzStringHead *head, LzStringError *error) {
    Scan s = {source->text, source->length};
    error->resume = SIZE_MAX;
    if (scan_string(&s, start, 0, head, error)) {
        return true;
    }
    if (error->resume == SIZE_MAX) {
        error->resume = head->is_multiline ? source->length
                                           : line_end_of(s.text, s.length, start);
    }
    return false;
}

bool lz_string_scan_braces(const LzSource *source, size_t open, bool multiline,
                           LzStringBraces *out, LzStringError *error) {
    Scan s = {source->text, source->length};
    error->resume = SIZE_MAX;
    return scan_braces(&s, open, 0, multiline, out, error);
}

/* ---- the reader ---- */

void lz_string_reader_start(LzStringReader *reader, const LzSource *source, const LzStringHead *head) {
    reader->source = source;
    reader->head = *head;
    reader->pos = head->body_start;
    reader->at_line_start = head->is_multiline;
}

bool lz_string_reader_init(LzStringReader *reader, const LzSource *source, size_t offset) {
    LzStringHead head;
    LzStringError error;
    if (!lz_string_scan(source, offset, &head, &error)) {
        return false;
    }
    lz_string_reader_start(reader, source, &head);
    return true;
}

static bool write_utf8(const LzWriter *writer, uint32_t scalar) {
    char bytes[4];
    size_t count;
    if (scalar < 0x80) {
        bytes[0] = (char)scalar;
        count = 1;
    } else if (scalar < 0x800) {
        bytes[0] = (char)(0xC0 | (scalar >> 6));
        bytes[1] = (char)(0x80 | (scalar & 0x3F));
        count = 2;
    } else if (scalar < 0x10000) {
        bytes[0] = (char)(0xE0 | (scalar >> 12));
        bytes[1] = (char)(0x80 | ((scalar >> 6) & 0x3F));
        bytes[2] = (char)(0x80 | (scalar & 0x3F));
        count = 3;
    } else {
        bytes[0] = (char)(0xF0 | (scalar >> 18));
        bytes[1] = (char)(0x80 | ((scalar >> 12) & 0x3F));
        bytes[2] = (char)(0x80 | ((scalar >> 6) & 0x3F));
        bytes[3] = (char)(0x80 | (scalar & 0x3F));
        count = 4;
    }
    return lz_writer_write(writer, bytes, count);
}

static void part_error(LzStringPart *part, const char *message, size_t offset, size_t length) {
    part->kind = LZ_STRING_ERROR;
    part->message = message;
    part->offset = offset;
    part->length = length;
}

/*
 * Handles the escape at reader->pos (a backslash). Returns 1 when something was done
 * (text written, or a line continuation), 0 after filling part with an error, -1 on
 * out of memory.
 */
static int read_escape(LzStringReader *reader, const LzWriter *writer, LzStringPart *part) {
    const char *text = reader->source->text;
    const LzStringHead *h = &reader->head;
    size_t at = reader->pos;
    size_t end = h->body_end;
    size_t p = at + 1;

    if (p >= end) {
        part_error(part, "invalid escape sequence: the backslash ends the string", at, 1);
        return 0;
    }

    if (h->is_multiline && (text[p] == '\n' || (text[p] == '\r' && p + 1 < end && text[p + 1] == '\n'))) {
        p += text[p] == '\r' ? 2 : 1;
        reader->pos = p;
        reader->at_line_start = true;
        return 1;
    }

    char simple = '\0';
    switch (text[p]) {
    case 'n':
        simple = '\n';
        break;
    case 't':
        simple = '\t';
        break;
    case 'r':
        simple = '\r';
        break;
    case '0':
        simple = '\0';
        break;
    case '\\':
    case '"':
    case '\'':
        simple = text[p];
        break;
    case 'x': {
        int high = p + 1 < end ? hex_value(text[p + 1]) : -1;
        int low = p + 2 < end ? hex_value(text[p + 2]) : -1;
        if (high < 0 || low < 0 || high > 7) {
            part_error(part, "invalid escape sequence (\\x takes two hex digits, 00 to 7F)", at, 2);
            return 0;
        }
        simple = (char)(high * 16 + low);
        p += 2;
        break;
    }
    case 'u': {
        size_t q = p + 1;
        uint32_t scalar = 0;
        int digits = 0;
        bool valid = false;
        if (q < end && text[q] == '{') {
            q++;
            while (q < end && hex_value(text[q]) >= 0 && digits < 7) {
                scalar = scalar * 16 + (uint32_t)hex_value(text[q]);
                q++;
                digits++;
            }
            valid = q < end && text[q] == '}' && digits >= 1 && digits <= 6 && scalar <= 0x10FFFF &&
                    !(scalar >= 0xD800 && scalar <= 0xDFFF);
        }
        if (!valid) {
            part_error(part, "invalid unicode escape (write \\u{1F600}: 1 to 6 hex digits)", at, 2);
            return 0;
        }
        reader->pos = q + 1;
        return write_utf8(writer, scalar) ? 1 : -1;
    }
    default:
        if (h->is_template && (text[p] == '{' || text[p] == '}')) {
            part_error(part, "invalid escape sequence: write '{{' or '}}' for a literal brace", at, 2);
        } else {
            size_t length = 2;
            while (at + length < end && ((unsigned char)text[at + length] & 0xC0) == 0x80) {
                length++;
            }
            part_error(part, "invalid escape sequence", at, length);
        }
        return 0;
    }

    reader->pos = p + 1;
    return lz_writer_write(writer, &simple, 1) ? 1 : -1;
}

static bool is_ordinary(char c, bool is_template, bool is_raw) {
    if (c == '\n' || c == '\r') {
        return false;
    }
    if (c == '\\') {
        return is_raw;
    }
    if (is_template && (c == '{' || c == '}')) {
        return false;
    }
    return true;
}

bool lz_string_reader_next(LzStringReader *reader, LzBuffer *out, LzStringPart *part) {
    const char *text = reader->source->text;
    const LzStringHead *h = &reader->head;
    LzWriter writer = lz_writer_from_buffer(out);
    size_t mark = out->length;
    memset(part, 0, sizeof *part);

    while (reader->pos < h->body_end) {
        if (reader->at_line_start) {
            reader->at_line_start = false;
            size_t q = reader->pos;
            while (q < h->body_end && is_blank(text[q])) {
                q++;
            }
            bool blank = q >= h->body_end || text[q] == '\n' ||
                         (text[q] == '\r' && q + 1 < h->body_end && text[q + 1] == '\n');
            size_t width = q - reader->pos;
            size_t common = 0;
            while (common < h->indent_length && common < width &&
                   text[reader->pos + common] == text[h->indent_start + common]) {
                common++;
            }
            if (blank) {
                reader->pos += common;
            } else if (common < h->indent_length) {
                part_error(part,
                           width < h->indent_length
                               ? "this line is indented less than the closing quotes"
                               : "this line's indentation does not match the closing quotes "
                                 "(tabs and spaces differ)",
                           reader->pos, width + 1);
                return true;
            } else {
                reader->pos += h->indent_length;
            }
            continue;
        }

        char c = text[reader->pos];
        if (c == '\n') {
            if (!lz_writer_write(&writer, "\n", 1)) {
                return false;
            }
            reader->pos++;
            reader->at_line_start = h->is_multiline;
            continue;
        }
        if (c == '\r') {
            if (h->is_multiline && reader->pos + 1 < h->body_end && text[reader->pos + 1] == '\n') {
                reader->pos++; /* CRLF is one line break */
                continue;
            }
            if (!lz_writer_write(&writer, "\r", 1)) {
                return false;
            }
            reader->pos++;
            continue;
        }
        if (c == '\\' && !h->is_raw) {
            int done = read_escape(reader, &writer, part);
            if (done < 0) {
                return false;
            }
            if (done == 0) {
                return true;
            }
            continue;
        }
        if (h->is_template && c == '}') {
            if (reader->pos + 1 < h->body_end && text[reader->pos + 1] == '}') {
                if (!lz_writer_write(&writer, "}", 1)) {
                    return false;
                }
                reader->pos += 2;
                continue;
            }
            part_error(part, "single '}' in a template string (write '}}' for a literal brace)",
                       reader->pos, 1);
            return true;
        }
        if (h->is_template && c == '{') {
            if (reader->pos + 1 < h->body_end && text[reader->pos + 1] == '{') {
                if (!lz_writer_write(&writer, "{", 1)) {
                    return false;
                }
                reader->pos += 2;
                continue;
            }
            if (out->length > mark) {
                part->kind = LZ_STRING_TEXT;
                return true;
            }

            size_t open = reader->pos;
            LzStringBraces braces;
            LzStringError error;
            if (!lz_string_scan_braces(reader->source, open, h->is_multiline, &braces, &error)) {
                part_error(part, error.message, error.offset, error.length);
                return true;
            }

            /* `{x=}`: a trailing '=' that is not part of ==, !=, <= or >=. */
            size_t trimmed = braces.expr_end;
            while (trimmed > open + 1 && (text[trimmed - 1] == ' ' || text[trimmed - 1] == '\t' ||
                                           text[trimmed - 1] == '\n' || text[trimmed - 1] == '\r')) {
                trimmed--;
            }
            bool debug = false;
            size_t expr_end = braces.expr_end;
            if (trimmed > open + 1 && text[trimmed - 1] == '=' &&
                !(trimmed - 1 > open + 1 && strchr("=!<>", text[trimmed - 2]) != NULL)) {
                debug = true;
                expr_end = trimmed - 1;
            }
            size_t first = open + 1;
            while (first < expr_end && (text[first] == ' ' || text[first] == '\t' || text[first] == '\n' ||
                                        text[first] == '\r')) {
                first++;
            }
            if (first >= expr_end) {
                part_error(part,
                           "empty '{}' in a template string (write '{{' and '}}' for literal braces)",
                           open, braces.close - open + 1);
                return true;
            }

            part->kind = LZ_STRING_EXPR;
            part->open = open;
            part->expr_start = open + 1;
            part->expr_end = expr_end;
            part->label_end = braces.expr_end;
            part->debug = debug;
            part->has_spec = braces.has_spec;
            part->spec_start = braces.spec_start;
            part->spec_end = braces.spec_end;
            part->multiline = h->is_multiline;
            reader->pos = braces.close + 1;
            return true;
        }

        size_t run = reader->pos;
        while (run < h->body_end && is_ordinary(text[run], h->is_template, h->is_raw)) {
            run++;
        }
        if (run == reader->pos) {
            run++; /* a raw string's backslash or similar: one byte at a time is always safe */
        }
        if (!lz_writer_write(&writer, text + reader->pos, run - reader->pos)) {
            return false;
        }
        reader->pos = run;
    }

    part->kind = out->length > mark ? LZ_STRING_TEXT : LZ_STRING_END;
    return true;
}
