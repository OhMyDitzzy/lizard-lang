#ifndef LIZARD_CHUNK_H
#define LIZARD_CHUNK_H

#include "lizard/value.h"

/*
 * Instructions are a byte stream: a one-byte opcode followed by its operands.
 * 16-bit operands are big-endian. Names used by GET_GLOBAL and DEFINE_GLOBAL
 * are string constants and must come from lz_vm_intern.
 */
typedef enum LzOpCode {
    LZ_OP_CONSTANT,      /* u16 constant index: push the constant */
    LZ_OP_NIL,           /* push nil */
    LZ_OP_TRUE,          /* push true */
    LZ_OP_FALSE,         /* push false */
    LZ_OP_POP,           /* discard the top value */
    LZ_OP_GET_GLOBAL,    /* u16 name index: push the global or builtin */
    LZ_OP_DEFINE_GLOBAL, /* u16 name index: pop the top value into a global */
    LZ_OP_CALL,          /* u8 argc: call the function below the arguments */
    LZ_OP_ADD,           /* pop b, pop a, push a + b (numbers or two strings) */
    LZ_OP_SUBTRACT,      /* pop b, pop a, push a - b */
    LZ_OP_MULTIPLY,      /* pop b, pop a, push a * b */
    LZ_OP_DIVIDE,        /* pop b, pop a, push a / b (always a float) */
    LZ_OP_FLOOR_DIVIDE,  /* pop b, pop a, push floor(a / b) */
    LZ_OP_MODULO,        /* pop b, pop a, push a % b, sign follows b */
    LZ_OP_NEGATE,        /* pop a, push -a */
    LZ_OP_EQUAL,         /* pop b, pop a, push a == b */
    LZ_OP_LESS,          /* pop b, pop a, push a < b (numbers only) */
    LZ_OP_GREATER,       /* pop b, pop a, push a > b (numbers only) */
    LZ_OP_NOT,           /* pop a, push true if a is falsy */
    LZ_OP_RETURN         /* stop executing the chunk */
} LzOpCode;

#define LZ_CONSTANTS_MAX 65536

typedef struct LzChunk {
    uint8_t *code;
    uint32_t *lines;
    size_t count;
    size_t capacity;
    LzValue *constants;
    size_t constant_count;
    size_t constant_capacity;
} LzChunk;

void lz_chunk_init(LzChunk *chunk);

/* Releases the references held by the constant table. */
void lz_chunk_free(LzHeap *heap, LzChunk *chunk);

bool lz_chunk_write(LzChunk *chunk, uint8_t byte, uint32_t line);
bool lz_chunk_write_u16(LzChunk *chunk, uint16_t operand, uint32_t line);

/* The chunk retains the value. Fails when out of memory or past LZ_CONSTANTS_MAX. */
bool lz_chunk_add_constant(LzChunk *chunk, LzValue value, uint16_t *index);

const char *lz_opcode_name(uint8_t opcode);

#endif
