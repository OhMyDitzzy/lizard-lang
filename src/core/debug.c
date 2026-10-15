#include "lizard/debug.h"

#include <stdarg.h>
#include <stdio.h>

static void put(const LzWriter *out, const char *format, ...) LZ_PRINTF_FORMAT(2, 3);

static void put(const LzWriter *out, const char *format, ...) {
    char text[160];
    va_list args;
    va_start(args, format);
    int length = vsnprintf(text, sizeof text, format, args);
    va_end(args);
    if (length > 0) {
        size_t size = (size_t)length < sizeof text ? (size_t)length : sizeof text - 1;
        (void)lz_writer_write(out, text, size);
    }
}

static size_t simple_instruction(const char *name, size_t offset, const LzWriter *out) {
    put(out, "%s\n", name);
    return offset + 1;
}

static size_t byte_instruction(const LzChunk *chunk, size_t offset, const LzWriter *out) {
    const char *name = lz_opcode_name(chunk->code[offset]);
    if (offset + 2 > chunk->count) {
        put(out, "%s <truncated>\n", name);
        return chunk->count;
    }
    put(out, "%-18s %5u\n", name, (unsigned)chunk->code[offset + 1]);
    return offset + 2;
}

static size_t constant_instruction(const LzChunk *chunk, size_t offset, const LzWriter *out) {
    const char *name = lz_opcode_name(chunk->code[offset]);
    if (offset + 3 > chunk->count) {
        put(out, "%s <truncated>\n", name);
        return chunk->count;
    }

    unsigned index = (unsigned)((chunk->code[offset + 1] << 8) | chunk->code[offset + 2]);
    put(out, "%-18s %5u '", name, index);
    if (index < chunk->constant_count) {
        (void)lz_value_write(&chunk->constants[index], out);
    } else {
        put(out, "<invalid>");
    }
    put(out, "'\n");
    return offset + 3;
}

size_t lz_disassemble_instruction(const LzChunk *chunk, size_t offset, const LzWriter *out) {
    uint32_t line = chunk->lines[offset];
    if (offset > 0 && line == chunk->lines[offset - 1]) {
        put(out, "%04zu    | ", offset);
    } else {
        put(out, "%04zu %4u ", offset, (unsigned)line);
    }

    uint8_t opcode = chunk->code[offset];
    switch (opcode) {
    case LZ_OP_CONSTANT:
    case LZ_OP_GET_GLOBAL:
    case LZ_OP_DEFINE_GLOBAL:
        return constant_instruction(chunk, offset, out);
    case LZ_OP_CALL:
        return byte_instruction(chunk, offset, out);
    case LZ_OP_NIL:
    case LZ_OP_TRUE:
    case LZ_OP_FALSE:
    case LZ_OP_POP:
    case LZ_OP_ADD:
    case LZ_OP_SUBTRACT:
    case LZ_OP_MULTIPLY:
    case LZ_OP_DIVIDE:
    case LZ_OP_FLOOR_DIVIDE:
    case LZ_OP_MODULO:
    case LZ_OP_NEGATE:
    case LZ_OP_EQUAL:
    case LZ_OP_LESS:
    case LZ_OP_GREATER:
    case LZ_OP_NOT:
    case LZ_OP_RETURN:
        return simple_instruction(lz_opcode_name(opcode), offset, out);
    default:
        put(out, "unknown opcode %u\n", (unsigned)opcode);
        return offset + 1;
    }
}

void lz_disassemble_chunk(const LzChunk *chunk, const char *name, const LzWriter *out) {
    put(out, "== %s ==\n", name);
    for (size_t offset = 0; offset < chunk->count;) {
        offset = lz_disassemble_instruction(chunk, offset, out);
    }
}
