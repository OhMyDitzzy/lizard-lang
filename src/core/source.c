#include "lizard/source.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void put(const LzWriter *out, const char *format, ...) LZ_PRINTF_FORMAT(2, 3);

/* For short formats only: the output is cut at 255 bytes. Long text is written directly. */
static void put(const LzWriter *out, const char *format, ...) {
    char text[256];
    va_list args;
    va_start(args, format);
    int length = vsnprintf(text, sizeof text, format, args);
    va_end(args);
    if (length > 0) {
        size_t size = (size_t)length < sizeof text ? (size_t)length : sizeof text - 1;
        (void)lz_writer_write(out, text, size);
    }
}

static bool is_continuation(char byte) {
    return ((unsigned char)byte & 0xC0) == 0x80;
}

void lz_source_from_buffer(LzSource *source, const char *name, const char *text, size_t length) {
    source->name = name;
    source->text = text;
    source->length = length;
    source->owned = false;
}

void lz_source_from_string(LzSource *source, const char *name, const char *text) {
    lz_source_from_buffer(source, name, text, strlen(text));
}

bool lz_source_read_file(LzSource *source, const char *path, char *error, size_t error_size) {
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        (void)snprintf(error, error_size, "cannot open '%s': %s", path, strerror(errno));
        return false;
    }

    size_t capacity = 4096;
    size_t length = 0;
    char *text = malloc(capacity + 1);
    while (text != NULL) {
        if (length == capacity) {
            if (capacity >= LZ_SOURCE_MAX_BYTES) {
                /* The buffer holds exactly the limit: only more data makes the file too large. */
                if (fgetc(file) == EOF) {
                    break;
                }
                (void)snprintf(error, error_size, "'%s' is larger than %u bytes", path,
                               (unsigned)LZ_SOURCE_MAX_BYTES);
                free(text);
                (void)fclose(file);
                return false;
            }
            capacity *= 2;
            if (capacity > LZ_SOURCE_MAX_BYTES) {
                capacity = LZ_SOURCE_MAX_BYTES;
            }
            char *grown = realloc(text, capacity + 1);
            if (grown == NULL) {
                free(text);
                text = NULL;
                break;
            }
            text = grown;
        }

        size_t got = fread(text + length, 1, capacity - length, file);
        length += got;
        if (got == 0) {
            break;
        }
    }

    bool failed = text == NULL || ferror(file);
    (void)fclose(file);
    if (failed) {
        (void)snprintf(error, error_size, "cannot read '%s'", path);
        free(text);
        return false;
    }

    /* A UTF-8 byte order mark is not program text; editors on Windows add one. */
    if (length >= 3 && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB &&
        (unsigned char)text[2] == 0xBF) {
        memmove(text, text + 3, length - 3);
        length -= 3;
    }

    text[length] = '\0';
    source->name = path;
    source->text = text;
    source->length = length;
    source->owned = true;
    return true;
}

void lz_source_free(LzSource *source) {
    if (source->owned) {
        free((void *)source->text);
    }
    source->text = NULL;
    source->length = 0;
    source->owned = false;
}

LzLocation lz_source_locate(const LzSource *source, size_t offset) {
    if (offset > source->length) {
        offset = source->length;
    }

    LzLocation location = {1, 1, 0, 0};
    for (size_t i = 0; i < offset; i++) {
        if (source->text[i] == '\n') {
            location.line++;
            location.line_start = i + 1;
        }
    }
    for (size_t i = location.line_start; i < offset; i++) {
        if (!is_continuation(source->text[i])) {
            location.column++;
        }
    }

    size_t end = offset;
    while (end < source->length && source->text[end] != '\n') {
        end++;
    }
    if (end > offset && source->text[end - 1] == '\r') {
        end--;
    }
    location.line_end = end;
    return location;
}

void lz_source_write_diagnostic(const LzSource *source, size_t offset, size_t length,
                                const char *severity, const char *message, const LzWriter *out) {
    if (offset > source->length) {
        offset = source->length;
    }
    LzLocation location = lz_source_locate(source, offset);

    /* Piece by piece: a long file name or message must never cut the header short. */
    (void)lz_writer_write_cstr(out, source->name);
    put(out, ":%u:%u: ", (unsigned)location.line, (unsigned)location.column);
    (void)lz_writer_write_cstr(out, severity);
    (void)lz_writer_write_cstr(out, ": ");
    (void)lz_writer_write_cstr(out, message);
    (void)lz_writer_write(out, "\n", 1);

    char number[16];
    int width = snprintf(number, sizeof number, "%u", (unsigned)location.line);
    put(out, " %s | ", number);
    (void)lz_writer_write(out, source->text + location.line_start,
                          location.line_end - location.line_start);
    (void)lz_writer_write(out, "\n", 1);

    put(out, " %*s | ", width, "");
    for (size_t i = location.line_start; i < offset; i++) {
        char byte = source->text[i];
        if (is_continuation(byte)) {
            continue;
        }
        (void)lz_writer_write(out, byte == '\t' ? "\t" : " ", 1);
    }

    size_t span_end = offset + length;
    if (span_end > location.line_end) {
        span_end = location.line_end;
    }
    size_t carets = 0;
    for (size_t i = offset; i < span_end; i++) {
        if (!is_continuation(source->text[i])) {
            carets++;
        }
    }
    if (carets == 0) {
        carets = 1;
    }
    for (size_t i = 0; i < carets; i++) {
        (void)lz_writer_write(out, "^", 1);
    }
    (void)lz_writer_write(out, "\n", 1);
}
