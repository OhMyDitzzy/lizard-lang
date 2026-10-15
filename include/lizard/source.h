#ifndef LIZARD_SOURCE_H
#define LIZARD_SOURCE_H

#include "lizard/common.h"
#include "lizard/writer.h"

#define LZ_SOURCE_MAX_BYTES (64u * 1024u * 1024u)

/*
 * Program text. Everything that points into source code (tokens, chunk
 * instructions, diagnostics) stores a byte offset into text; lines and
 * columns are computed from the text only when an error is reported.
 */
typedef struct LzSource {
    const char *name;
    const char *text;
    size_t length;
    bool owned;
} LzSource;

/* 1-based line and column; column counts UTF-8 code points, not bytes. */
typedef struct LzLocation {
    uint32_t line;
    uint32_t column;
    size_t line_start;
    size_t line_end; /* excludes the line break */
} LzLocation;

/* Borrow text the caller keeps alive; name is used in diagnostics. */
void lz_source_from_buffer(LzSource *source, const char *name, const char *text, size_t length);
void lz_source_from_string(LzSource *source, const char *name, const char *text);

/* Reads a whole file. name borrows path. On failure writes a message to error. */
bool lz_source_read_file(LzSource *source, const char *path, char *error, size_t error_size);

void lz_source_free(LzSource *source);

/* offset is clamped to the end of the text. */
LzLocation lz_source_locate(const LzSource *source, size_t offset);

/*
 * Writes "name:line:col: severity: message", the source line, and a caret
 * underline for the span [offset, offset + length), clipped to its first line.
 */
void lz_source_write_diagnostic(const LzSource *source, size_t offset, size_t length,
                                const char *severity, const char *message, const LzWriter *out);

#endif
