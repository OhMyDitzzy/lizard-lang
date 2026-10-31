#ifndef LIZARD_STRLIT_H
#define LIZARD_STRLIT_H

#include "lizard/source.h"

/*
 * String literals. One form covers every kind:
 *
 *     [$][r] fence body fence
 *
 *   $      template: {expr}, {expr:spec} and {expr=} are replaced by values
 *   r      raw: no escape sequences
 *   fence  one quote (single line), or three or more quotes (see below)
 *
 * A fence of three or more quotes followed by a line break opens a multi-line string:
 * it ends at a line that holds only the same number of quotes (after indentation), the
 * indentation of that line is removed from every line, and the first and last line
 * breaks are not part of the text. A long fence followed by text on the same line is a
 * single-line string that may contain quotes; it ends at a run of exactly as many quotes.
 *
 * The lexer finds a literal with lz_string_scan and checks it with a reader; the compiler
 * walks a template with the same reader, so both agree on what a literal means.
 */

typedef struct LzStringHead {
    bool is_template;
    bool is_raw;
    bool is_multiline;
    bool needs_check; /* has escapes, a template or lines: decoding can fail */
    char quote;
    size_t fence;        /* quotes in the fence */
    size_t body_start;   /* first byte of the text */
    size_t body_end;     /* one past the last byte of the text */
    size_t indent_start; /* multi-line: the indentation of the closing line */
    size_t indent_length;
    size_t end; /* one past the closing fence */
} LzStringHead;

typedef struct LzStringError {
    const char *message; /* static */
    size_t offset;
    size_t length;
    size_t resume; /* where the lexer continues */
} LzStringError;

/* The span of one `{...}` of a template. */
typedef struct LzStringBraces {
    size_t expr_end; /* the ':' or '}' that ends the expression */
    bool has_spec;
    size_t spec_start;
    size_t spec_end;
    size_t close; /* the closing '}' */
} LzStringBraces;

/* True when a string literal can start at this offset (a quote, `$`, or `r` before a quote). */
bool lz_string_starts_at(const LzSource *source, size_t at);

/* Finds the end of the literal that starts at start. */
bool lz_string_scan(const LzSource *source, size_t start, LzStringHead *head, LzStringError *error);

/* Scans the braces whose '{' is at open (used for dynamic widths in a format spec). */
bool lz_string_scan_braces(const LzSource *source, size_t open, bool multiline,
                           LzStringBraces *out, LzStringError *error);

typedef enum LzStringPartKind {
    LZ_STRING_END,
    LZ_STRING_TEXT, /* decoded text was appended to the buffer */
    LZ_STRING_EXPR,
    LZ_STRING_ERROR
} LzStringPartKind;

typedef struct LzStringPart {
    LzStringPartKind kind;
    /* EXPR */
    size_t open;       /* the '{' */
    size_t expr_start; /* the expression is [expr_start, expr_end) */
    size_t expr_end;
    size_t label_end;  /* debug form: the text shown is [open + 1, label_end) */
    bool debug;
    bool has_spec;
    size_t spec_start; /* [spec_start, spec_end) */
    size_t spec_end;
    bool multiline;
    /* ERROR */
    const char *message;
    size_t offset;
    size_t length;
} LzStringPart;

typedef struct LzStringReader {
    const LzSource *source;
    LzStringHead head;
    size_t pos;
    bool at_line_start;
} LzStringReader;

void lz_string_reader_start(LzStringReader *reader, const LzSource *source, const LzStringHead *head);

/* Scans the literal at offset and starts reading it. False if it does not scan. */
bool lz_string_reader_init(LzStringReader *reader, const LzSource *source, size_t offset);

/*
 * The next part of the literal. Text is appended to out (one TEXT part per run between
 * expressions). Returns false only when memory runs out.
 */
bool lz_string_reader_next(LzStringReader *reader, LzBuffer *out, LzStringPart *part);

#endif
