#ifndef LIZARD_WRITER_H
#define LIZARD_WRITER_H

#include <stdio.h>

#include "lizard/common.h"

/*
 * Output sink used by the VM. Builtins never touch stdout directly, so tests
 * can capture output and a host can redirect it later.
 */
typedef struct LzWriter {
    bool (*write)(void *ctx, const char *data, size_t length);
    void (*flush)(void *ctx);
    void *ctx;
} LzWriter;

typedef struct LzBuffer {
    char *data;
    size_t length;
    size_t capacity;
} LzBuffer;

LzWriter lz_writer_from_file(FILE *file);
LzWriter lz_writer_from_buffer(LzBuffer *buffer);

bool lz_writer_write(const LzWriter *writer, const char *data, size_t length);
bool lz_writer_write_cstr(const LzWriter *writer, const char *text);
void lz_writer_flush(const LzWriter *writer);

void lz_buffer_free(LzBuffer *buffer);

#endif
