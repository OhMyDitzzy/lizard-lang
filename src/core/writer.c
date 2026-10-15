#include "lizard/writer.h"

#include <stdlib.h>
#include <string.h>

static bool file_write(void *ctx, const char *data, size_t length) {
    return fwrite(data, 1, length, (FILE *)ctx) == length;
}

static void file_flush(void *ctx) {
    (void)fflush((FILE *)ctx);
}

static bool buffer_write(void *ctx, const char *data, size_t length) {
    LzBuffer *buffer = ctx;
    size_t needed = buffer->length + length + 1;

    if (needed > buffer->capacity) {
        size_t capacity = buffer->capacity != 0 ? buffer->capacity : 64;
        while (capacity < needed) {
            capacity *= 2;
        }
        char *grown = realloc(buffer->data, capacity);
        if (grown == NULL) {
            return false;
        }
        buffer->data = grown;
        buffer->capacity = capacity;
    }

    if (length > 0) {
        memcpy(buffer->data + buffer->length, data, length);
    }
    buffer->length += length;
    buffer->data[buffer->length] = '\0';
    return true;
}

LzWriter lz_writer_from_file(FILE *file) {
    LzWriter writer = {file_write, file_flush, file};
    return writer;
}

LzWriter lz_writer_from_buffer(LzBuffer *buffer) {
    LzWriter writer = {buffer_write, NULL, buffer};
    return writer;
}

bool lz_writer_write(const LzWriter *writer, const char *data, size_t length) {
    if (writer->write == NULL) {
        return false;
    }
    if (length == 0) {
        return true;
    }
    return writer->write(writer->ctx, data, length);
}

bool lz_writer_write_cstr(const LzWriter *writer, const char *text) {
    return lz_writer_write(writer, text, strlen(text));
}

void lz_writer_flush(const LzWriter *writer) {
    if (writer->flush != NULL) {
        writer->flush(writer->ctx);
    }
}

void lz_buffer_free(LzBuffer *buffer) {
    free(buffer->data);
    buffer->data = NULL;
    buffer->length = 0;
    buffer->capacity = 0;
}
