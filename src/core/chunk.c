#include "lizard/chunk.h"

#include <stdlib.h>

void lz_chunk_init(LzChunk *chunk) {
    chunk->code = NULL;
    chunk->offsets = NULL;
    chunk->count = 0;
    chunk->capacity = 0;
    chunk->constants = NULL;
    chunk->constant_count = 0;
    chunk->constant_capacity = 0;
}

void lz_chunk_free(LzHeap *heap, LzChunk *chunk) {
    for (size_t i = 0; i < chunk->constant_count; i++) {
        lz_value_release(heap, &chunk->constants[i]);
    }
    free(chunk->code);
    free(chunk->offsets);
    free(chunk->constants);
    lz_chunk_init(chunk);
}

static bool reserve_code(LzChunk *chunk, size_t extra) {
    size_t needed = chunk->count + extra;
    if (needed <= chunk->capacity) {
        return true;
    }

    size_t capacity = chunk->capacity != 0 ? chunk->capacity : 64;
    while (capacity < needed) {
        capacity *= 2;
    }

    uint8_t *code = realloc(chunk->code, capacity);
    if (code == NULL) {
        return false;
    }
    chunk->code = code;

    uint32_t *offsets = realloc(chunk->offsets, capacity * sizeof *offsets);
    if (offsets == NULL) {
        return false;
    }
    chunk->offsets = offsets;
    chunk->capacity = capacity;
    return true;
}

bool lz_chunk_write(LzChunk *chunk, uint8_t byte, uint32_t source_offset) {
    if (!reserve_code(chunk, 1)) {
        return false;
    }
    chunk->code[chunk->count] = byte;
    chunk->offsets[chunk->count] = source_offset;
    chunk->count++;
    return true;
}

bool lz_chunk_write_u16(LzChunk *chunk, uint16_t operand, uint32_t source_offset) {
    if (!reserve_code(chunk, 2)) {
        return false;
    }
    return lz_chunk_write(chunk, (uint8_t)(operand >> 8), source_offset) &&
           lz_chunk_write(chunk, (uint8_t)(operand & 0xFF), source_offset);
}

bool lz_chunk_add_constant(LzChunk *chunk, LzValue value, uint16_t *index) {
    if (chunk->constant_count >= LZ_CONSTANTS_MAX) {
        return false;
    }

    if (chunk->constant_count == chunk->constant_capacity) {
        size_t capacity = chunk->constant_capacity != 0 ? chunk->constant_capacity * 2 : 8;
        LzValue *grown = realloc(chunk->constants, capacity * sizeof *grown);
        if (grown == NULL) {
            return false;
        }
        chunk->constants = grown;
        chunk->constant_capacity = capacity;
    }

    lz_value_retain(&value);
    chunk->constants[chunk->constant_count] = value;
    *index = (uint16_t)chunk->constant_count;
    chunk->constant_count++;
    return true;
}

const char *lz_opcode_name(uint8_t opcode) {
    switch (opcode) {
    case LZ_OP_CONSTANT:
        return "OP_CONSTANT";
    case LZ_OP_NIL:
        return "OP_NIL";
    case LZ_OP_TRUE:
        return "OP_TRUE";
    case LZ_OP_FALSE:
        return "OP_FALSE";
    case LZ_OP_POP:
        return "OP_POP";
    case LZ_OP_GET_GLOBAL:
        return "OP_GET_GLOBAL";
    case LZ_OP_DEFINE_GLOBAL:
        return "OP_DEFINE_GLOBAL";
    case LZ_OP_CALL:
        return "OP_CALL";
    case LZ_OP_ADD:
        return "OP_ADD";
    case LZ_OP_SUBTRACT:
        return "OP_SUBTRACT";
    case LZ_OP_MULTIPLY:
        return "OP_MULTIPLY";
    case LZ_OP_DIVIDE:
        return "OP_DIVIDE";
    case LZ_OP_FLOOR_DIVIDE:
        return "OP_FLOOR_DIVIDE";
    case LZ_OP_MODULO:
        return "OP_MODULO";
    case LZ_OP_NEGATE:
        return "OP_NEGATE";
    case LZ_OP_EQUAL:
        return "OP_EQUAL";
    case LZ_OP_LESS:
        return "OP_LESS";
    case LZ_OP_GREATER:
        return "OP_GREATER";
    case LZ_OP_NOT:
        return "OP_NOT";
    case LZ_OP_RETURN:
        return "OP_RETURN";
    default:
        return "OP_UNKNOWN";
    }
}
