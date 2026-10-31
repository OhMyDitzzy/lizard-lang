#ifndef LIZARD_CHUNK_H
#define LIZARD_CHUNK_H

#include "lizard/type.h"
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
    LZ_OP_LESS_EQUAL,    /* pop b, pop a, push a <= b (false with NaN) */
    LZ_OP_GREATER_EQUAL, /* pop b, pop a, push a >= b (false with NaN) */
    LZ_OP_NOT,           /* pop a, push true if a is falsy */
    LZ_OP_GET_LOCAL,     /* u16 slot: push a copy of stack[slot] */
    LZ_OP_SET_LOCAL,     /* u16 slot: pop the top value into stack[slot] */
    LZ_OP_SET_GLOBAL,    /* u16 name index: pop the top value into an existing global */
    LZ_OP_JUMP,          /* u16 distance: skip forward */
    LZ_OP_JUMP_IF_FALSE, /* u16 distance: skip forward if the top is falsy; does not pop */
    LZ_OP_JUMP_IF_TRUE,  /* u16 distance: skip forward if the top is truthy; does not pop */
    LZ_OP_LOOP,          /* u16 distance: jump backward */
    LZ_OP_CHECK_TYPE,    /* u16 type index: fail with LZ_ERR_TYPE unless the top value matches */
    LZ_OP_ARRAY_NEW,     /* push a new empty array */
    LZ_OP_ARRAY_PUSH,    /* pop a value and append it to the array below it (which stays) */
    LZ_OP_INDEX,         /* pop index, pop array, push array[index] (negative counts from the end) */
    LZ_OP_SET_INDEX,     /* pop value, pop index, pop array: array[index] = value */
    LZ_OP_SLICE,         /* pop end, pop start, pop array, push a new array (nil bound = open) */
    LZ_OP_RETURN,        /* top level: stop executing. In a function: return the top value to the caller */

    /* Functions. Kept after OP_RETURN so the original opcodes keep their numbers. */
    LZ_OP_CALL_EX,       /* u8 named, u16 names: call with [callee][array of positional args][named values] */
    LZ_OP_CLOSURE,       /* u16 constant (a function): push a closure that captures its upvalues */
    LZ_OP_GET_UPVALUE,   /* u16 index: push a captured variable of the running closure */
    LZ_OP_SET_UPVALUE,   /* u16 index: pop the top value into a captured variable */
    LZ_OP_CLOSE_UPVALUE, /* close the upvalues at the top slot, then pop it (a captured local going out of scope) */
    LZ_OP_GET_SELF,      /* push the running closure (a function calling itself by name) */
    LZ_OP_CHECK_PARAM,   /* u16 slot, u16 type: fail with LZ_ERR_TYPE unless the parameter matches */
    LZ_OP_CHECK_RETURN,  /* u16 type: fail with LZ_ERR_TYPE unless the value about to be returned matches */
    LZ_OP_JUMP_IF_SUPPLIED, /* u16 parameter, u16 distance: skip forward if the caller supplied that argument */
    LZ_OP_ARRAY_EXTEND,  /* pop an array and append all its items to the array below it (which stays) */
    LZ_OP_SWAP,          /* exchange the two values on top (a method call puts its callee under the receiver) */

    /* Bitwise operators (ints only). Appended last so the earlier opcodes keep their numbers. */
    LZ_OP_BIT_AND,       /* pop b, pop a, push a & b */
    LZ_OP_BIT_OR,        /* pop b, pop a, push a | b */
    LZ_OP_BIT_XOR,       /* pop b, pop a, push a ^ b */
    LZ_OP_BIT_NOT,       /* pop a, push ~a */
    LZ_OP_SHL,           /* pop n, pop a, push a << n (0..63; bits shifted out are dropped) */
    LZ_OP_SHR            /* pop n, pop a, push a >> n (0..63; arithmetic shift) */
} LzOpCode;

#define LZ_CONSTANTS_MAX 65536
#define LZ_TYPES_MAX 65536

typedef struct LzChunk {
    uint8_t *code;
    uint32_t *offsets; /* source offset of each byte, see lizard/source.h */
    size_t count;
    size_t capacity;
    LzValue *constants;
    size_t constant_count;
    size_t constant_capacity;
    LzType *types; /* referenced by OP_CHECK_TYPE */
    size_t type_count;
    size_t type_capacity;
    uint32_t *type_slots; /* open-addressing index into types, 0 = empty, else index + 1 */
    size_t type_slot_capacity;
} LzChunk;

void lz_chunk_init(LzChunk *chunk);

/* Releases the references held by the constant table. */
void lz_chunk_free(LzHeap *heap, LzChunk *chunk);

/* Frees the arrays without releasing the constants (for a heap that frees every object itself). */
void lz_chunk_discard(LzChunk *chunk);

bool lz_chunk_write(LzChunk *chunk, uint8_t byte, uint32_t source_offset);
bool lz_chunk_write_u16(LzChunk *chunk, uint16_t operand, uint32_t source_offset);

/* The chunk retains the value. Fails when out of memory or past LZ_CONSTANTS_MAX. */
bool lz_chunk_add_constant(LzChunk *chunk, LzValue value, uint16_t *index);

/* Stores a type once; an identical type already in the table is reused. */
bool lz_chunk_add_type(LzChunk *chunk, const LzType *type, uint16_t *index);

const char *lz_opcode_name(uint8_t opcode);

/* Total size of an instruction in bytes, or 0 for an unknown opcode. */
size_t lz_opcode_length(uint8_t opcode);

/* Drops code and constants added after the given marks, releasing those constants. */
void lz_chunk_truncate(LzHeap *heap, LzChunk *chunk, size_t code_count, size_t constant_count);

#endif
