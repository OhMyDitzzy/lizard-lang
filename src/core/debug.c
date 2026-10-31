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

static size_t slot_instruction(const LzChunk *chunk, size_t offset, const LzWriter *out) {
    const char *name = lz_opcode_name(chunk->code[offset]);
    if (offset + 3 > chunk->count) {
        put(out, "%s <truncated>\n", name);
        return chunk->count;
    }
    unsigned slot = (unsigned)((chunk->code[offset + 1] << 8) | chunk->code[offset + 2]);
    put(out, "%-18s %5u\n", name, slot);
    return offset + 3;
}

static size_t jump_instruction(const LzChunk *chunk, size_t offset, bool forward,
                               const LzWriter *out) {
    const char *name = lz_opcode_name(chunk->code[offset]);
    if (offset + 3 > chunk->count) {
        put(out, "%s <truncated>\n", name);
        return chunk->count;
    }
    size_t distance = (size_t)((chunk->code[offset + 1] << 8) | chunk->code[offset + 2]);
    size_t after = offset + 3;
    size_t target = forward ? after + distance : (distance <= after ? after - distance : 0);
    put(out, "%-18s %5zu -> %04zu\n", name, distance, target);
    return offset + 3;
}

static size_t type_instruction(const LzChunk *chunk, size_t offset, const LzWriter *out) {
    const char *name = lz_opcode_name(chunk->code[offset]);
    if (offset + 3 > chunk->count) {
        put(out, "%s <truncated>\n", name);
        return chunk->count;
    }

    unsigned index = (unsigned)((chunk->code[offset + 1] << 8) | chunk->code[offset + 2]);
    put(out, "%-18s %5u '", name, index);
    if (index < chunk->type_count) {
        lz_type_describe(&chunk->types[index], out);
    } else {
        put(out, "<invalid>");
    }
    put(out, "'\n");
    return offset + 3;
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

static unsigned read16(const LzChunk *chunk, size_t at) {
    return (unsigned)((chunk->code[at] << 8) | chunk->code[at + 1]);
}

/* Two u16 operands: CHECK_PARAM slot type, JUMP_IF_SUPPLIED parameter distance. */
static size_t pair_instruction(const LzChunk *chunk, size_t offset, const LzWriter *out) {
    uint8_t opcode = chunk->code[offset];
    const char *name = lz_opcode_name(opcode);
    if (offset + 5 > chunk->count) {
        put(out, "%s <truncated>\n", name);
        return chunk->count;
    }
    unsigned first = read16(chunk, offset + 1);
    unsigned second = read16(chunk, offset + 3);
    if (opcode == LZ_OP_JUMP_IF_SUPPLIED) {
        put(out, "%-18s %5u %5u -> %04zu\n", name, first, second, offset + 5 + second);
        return offset + 5;
    }
    put(out, "%-18s %5u '", name, first);
    if (second < chunk->type_count) {
        lz_type_describe(&chunk->types[second], out);
    } else {
        put(out, "<invalid>");
    }
    put(out, "'\n");
    return offset + 5;
}

static size_t call_ex_instruction(const LzChunk *chunk, size_t offset, const LzWriter *out) {
    const char *name = lz_opcode_name(chunk->code[offset]);
    if (offset + 4 > chunk->count) {
        put(out, "%s <truncated>\n", name);
        return chunk->count;
    }
    put(out, "%-18s %5u named\n", name, (unsigned)chunk->code[offset + 1]);
    return offset + 4;
}

static uint32_t shown_position(const LzChunk *chunk, size_t index, const LzSource *source) {
    uint32_t position = chunk->offsets[index];
    return source != NULL ? lz_source_locate(source, position).line : position;
}

size_t lz_disassemble_instruction(const LzChunk *chunk, size_t offset, const LzSource *source,
                                  const LzWriter *out) {
    uint32_t shown = shown_position(chunk, offset, source);
    if (offset > 0 && shown == shown_position(chunk, offset - 1, source)) {
        put(out, "%04zu    | ", offset);
    } else {
        put(out, "%04zu %4u ", offset, (unsigned)shown);
    }

    uint8_t opcode = chunk->code[offset];
    switch (opcode) {
    case LZ_OP_CONSTANT:
    case LZ_OP_GET_GLOBAL:
    case LZ_OP_DEFINE_GLOBAL:
    case LZ_OP_SET_GLOBAL:
    case LZ_OP_CLOSURE:
        return constant_instruction(chunk, offset, out);
    case LZ_OP_GET_LOCAL:
    case LZ_OP_SET_LOCAL:
    case LZ_OP_GET_UPVALUE:
    case LZ_OP_SET_UPVALUE:
        return slot_instruction(chunk, offset, out);
    case LZ_OP_CHECK_TYPE:
    case LZ_OP_CHECK_RETURN:
        return type_instruction(chunk, offset, out);
    case LZ_OP_CHECK_PARAM:
    case LZ_OP_JUMP_IF_SUPPLIED:
        return pair_instruction(chunk, offset, out);
    case LZ_OP_CALL_EX:
        return call_ex_instruction(chunk, offset, out);
    case LZ_OP_JUMP:
    case LZ_OP_JUMP_IF_FALSE:
    case LZ_OP_JUMP_IF_TRUE:
        return jump_instruction(chunk, offset, true, out);
    case LZ_OP_LOOP:
        return jump_instruction(chunk, offset, false, out);
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
    case LZ_OP_LESS_EQUAL:
    case LZ_OP_GREATER_EQUAL:
    case LZ_OP_NOT:
    case LZ_OP_ARRAY_NEW:
    case LZ_OP_ARRAY_PUSH:
    case LZ_OP_INDEX:
    case LZ_OP_SET_INDEX:
    case LZ_OP_SLICE:
    case LZ_OP_RETURN:
    case LZ_OP_CLOSE_UPVALUE:
    case LZ_OP_GET_SELF:
    case LZ_OP_ARRAY_EXTEND:
    case LZ_OP_SWAP:
    case LZ_OP_BIT_AND:
    case LZ_OP_BIT_OR:
    case LZ_OP_BIT_XOR:
    case LZ_OP_BIT_NOT:
    case LZ_OP_SHL:
    case LZ_OP_SHR:
        return simple_instruction(lz_opcode_name(opcode), offset, out);
    default:
        put(out, "unknown opcode %u\n", (unsigned)opcode);
        return offset + 1;
    }
}

void lz_disassemble_chunk(const LzChunk *chunk, const char *name, const LzSource *source,
                          const LzWriter *out) {
    put(out, "== %s ==\n", name);
    for (size_t offset = 0; offset < chunk->count;) {
        offset = lz_disassemble_instruction(chunk, offset, source, out);
    }
}
