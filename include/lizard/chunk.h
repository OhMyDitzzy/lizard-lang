#ifndef LIZARD_CHUNK_H
#define LIZARD_CHUNK_H

#include "lizard/type.h"
#include "lizard/value.h"

/*
 * Instructions are a byte stream with a one-byte opcode followed by its operands. 16-bit operands
 * are big-endian. Names used by GET_GLOBAL and DEFINE_GLOBAL are string constants and must come
 * from lz_vm_intern.
 */
typedef enum LzOpCode {
    LZ_OP_CONSTANT,      /* u16 constant index, pushes the constant */
    LZ_OP_NIL,           /* push nil */
    LZ_OP_TRUE,          /* push true */
    LZ_OP_FALSE,         /* push false */
    LZ_OP_POP,           /* discard the top value */
    LZ_OP_GET_GLOBAL,    /* u16 name index: push the global or builtin */
    LZ_OP_DEFINE_GLOBAL, /* u16 name index: pop the top value into a global */
    LZ_OP_CALL,          /* u8 argc, calls the function below the arguments */
    LZ_OP_ADD,           /* pops b and a, pushes a + b. Works on numbers or two strings */
    LZ_OP_SUBTRACT,      /* pop b, pop a, push a - b */
    LZ_OP_MULTIPLY,      /* pop b, pop a, push a * b */
    LZ_OP_DIVIDE,        /* pops b and a, pushes a / b, which is always a float */
    LZ_OP_FLOOR_DIVIDE,  /* pop b, pop a, push floor(a / b) */
    LZ_OP_MODULO,        /* pops b and a, pushes a % b, and the sign follows b */
    LZ_OP_NEGATE,        /* pop a, push -a */
    LZ_OP_EQUAL,         /* pop b, pop a, push a == b */
    LZ_OP_LESS,          /* pops b and a, pushes a < b. Numbers only */
    LZ_OP_GREATER,       /* pops b and a, pushes a > b. Numbers only */
    LZ_OP_LESS_EQUAL,    /* pops b and a, pushes a <= b, which is false with NaN */
    LZ_OP_GREATER_EQUAL, /* pops b and a, pushes a >= b, which is false with NaN */
    LZ_OP_NOT,           /* pops a, pushes true if a is falsy */
    LZ_OP_GET_LOCAL,     /* u16 slot: push a copy of stack[slot] */
    LZ_OP_SET_LOCAL,     /* u16 slot: pop the top value into stack[slot] */
    LZ_OP_SET_GLOBAL,    /* u16 name index: pop the top value into an existing global */
    LZ_OP_JUMP,          /* u16 distance, skips forward */
    LZ_OP_JUMP_IF_FALSE, /* u16 distance, skips forward if the top is falsy and does not pop */
    LZ_OP_JUMP_IF_TRUE,  /* u16 distance, skips forward if the top is truthy and does not pop */
    LZ_OP_LOOP,          /* u16 distance, jumps backward */
    LZ_OP_CHECK_TYPE,    /* u16 type index, fails with LZ_ERR_TYPE unless the top value matches */
    LZ_OP_ARRAY_NEW,     /* push a new empty array */
    LZ_OP_ARRAY_PUSH,    /* pops a value and appends it to the array below it, which stays */
    LZ_OP_INDEX,         /* pops index and array, pushes array[index]. A negative index counts from the end */
    LZ_OP_SET_INDEX,     /* pops value, index and array, then does array[index] = value */
    LZ_OP_SLICE,         /* pops end, start and array, pushes a new array. A nil bound means open */
    LZ_OP_RETURN,        /* at top level it stops executing, and in a function it returns the top value to the caller */

    /* Functions. They come after OP_RETURN so the original opcodes keep their numbers. */
    LZ_OP_CALL_EX,       /* u8 named, u16 names, calls with [callee][array of positional args][named values] */
    LZ_OP_CLOSURE,       /* u16 constant that is a function, pushes a closure that captures its upvalues */
    LZ_OP_GET_UPVALUE,   /* u16 index: push a captured variable of the running closure */
    LZ_OP_SET_UPVALUE,   /* u16 index: pop the top value into a captured variable */
    LZ_OP_CLOSE_UPVALUE, /* closes the upvalues at the top slot and then pops it, for a captured local going out of scope */
    LZ_OP_GET_SELF,      /* pushes the running closure, for a function calling itself by name */
    LZ_OP_CHECK_PARAM,   /* u16 slot, u16 type, fails with LZ_ERR_TYPE unless the parameter matches */
    LZ_OP_CHECK_RETURN,  /* u16 type, fails with LZ_ERR_TYPE unless the value about to be returned matches */
    LZ_OP_JUMP_IF_SUPPLIED, /* u16 parameter, u16 distance, skips forward if the caller supplied that argument */
    LZ_OP_ARRAY_EXTEND,  /* pops an array and appends all its items to the array below it, which stays */
    LZ_OP_SWAP,          /* swaps the two values on top, because a method call puts its callee under the receiver */

    /*
     * Bitwise operators, ints only. They were added last so the earlier opcodes keep their
     * numbers.
     */
    LZ_OP_BIT_AND,       /* pop b, pop a, push a & b */
    LZ_OP_BIT_OR,        /* pop b, pop a, push a | b */
    LZ_OP_BIT_XOR,       /* pop b, pop a, push a ^ b */
    LZ_OP_BIT_NOT,       /* pop a, push ~a */
    LZ_OP_SHL,           /* pops n and a, pushes a << n. n is 0 to 63 and bits shifted out are dropped */
    LZ_OP_SHR,           /* pops n and a, pushes a >> n. n is 0 to 63 and it's an arithmetic shift */

    /* Compound index assignment. Added last so the earlier opcodes keep their numbers. */
    LZ_OP_INDEX_KEEP,     /* pops index, keeps the array and pushes array[index], so [a][i] becomes [a][i][item] */

    /* `for ... in` takes one item per round, and no array is built for the string case. */
    LZ_OP_ITER_LENGTH,     /* pops x and pushes how many rounds it has, which is the items of an array or the bytes of a
string. Anything else fails with "cannot iterate over <kind>". */
    LZ_OP_ITER_NEXT,       /* pops off, keeps x and pushes the item and then the next offset, so [x][off] becomes
[x][off][item][next]. Strings step by code point */
    LZ_OP_RANGE_TEST,      /* u8 inclusive, pops step, limit and counter and pushes whether the counter is still inside.
A step of 0 is an error. */
    LZ_OP_RANGE_LAST,      /* u8 inclusive, pops step, limit and counter and pushes the last value the loop visits. It only
runs once the test said the range is not empty, so the last value is reachable without overflow. */
    LZ_OP_RANGE_NEXT,      /* pops step and counter and pushes counter + step. It never overflows because the loop only
steps while the counter is before the last value */
    LZ_OP_CLOSE_UPVALUE_KEEP, /* closes the upvalue of the top slot and keeps the slot, because the captured loop variable of
a C-style `for` is fresh each round */

    /* Dicts. Added last so the earlier opcodes keep their numbers. */
    LZ_OP_DICT,        /* u16 count, pops 2*count values as key and value pairs and pushes a new dict */
    LZ_OP_ITER_PAIR,   /* u16 counter slot, turns [x][off] into [x][first][second][next]. An array or a string gives
the round and the item, and a dict gives the key and the value */
    LZ_OP_ITER_CHECK,  /* pops size and x. Fails with "dict changed size during iteration" when x is a dict whose size
is no longer size, and does nothing for anything else */

    /* `in`, `not in` and `??`. Added last so the earlier opcodes keep their numbers. */
    LZ_OP_IN,          /* pops b and a and pushes whether a is in b, meaning an item of an array, a key of a dict or a
substring of a string. Anything else on the right is an error */
    LZ_OP_IN_RANGE,    /* u8 flags where 1 is inclusive and 2 is lenient, pops b, a and x and pushes a <= x < b, or
a <= x <= b when inclusive. The bounds must be ints and x a number, unless lenient, in which
case anything but a number is simply outside */
    LZ_OP_NIL_TEST,    /* keeps x and pushes whether it is nil, so [x] becomes [x][is nil] */

    LZ_OP_DUP,         /* pushes a copy of the top, so [x] becomes [x][x] */

    LZ_OP_TRY_BEGIN,   /* u16 forward offset of the catch code. It opens a handler that remembers the frame and the
stack height. An error inside the frame, or in what it calls, cuts the stack back to that
height, pushes the error value and continues at the catch code */
    LZ_OP_TRY_END,     /* closes the innermost handler, because the try ended without an error */
    LZ_OP_THROW,       /* pops an error value and fails with it. A thrown value keeps its trace */
    LZ_OP_ERROR_IS,    /* u8 error kind, pops an error and pushes whether it is of that kind or below */
    LZ_OP_GET_FIELD,   /* u16 constant name, pops an error or an object and pushes its field, one of message, kind or trace */
    /* Classes. Added last so the earlier opcodes keep their numbers. */
    LZ_OP_GET_FIELD_KEEP, /* u16 constant name, keeps the object and pushes its field, so [o] becomes [o][field] */
    LZ_OP_SET_FIELD,   /* u16 constant name, pops value and object and does object.name = value */
    LZ_OP_METHOD_LOOKUP, /* u16 name, u8 mode, turns [receiver][fallback] into [receiver][callee]. An object answers with
its class's member. Anything else keeps the fallback in mode 0, or finds a function of that
name in mode 1, where the input is [receiver][nil] */
    LZ_OP_GET_FIELD_W, /* u16 constant name, like GET_FIELD for a value that is about to be changed in place, such as
`o.items[i] = v`. A read-only field of another class is refused */
    LZ_OP_CLASS_INIT,  /* u16 constant that is a class, runs the initialisers of its constants once */
    LZ_OP_IS,          /* u16 type index, replaces the top value by a bool saying whether it matches that type. This is
the test of an annotation `: T` */
    LZ_OP_SUPER,       /* u16 constant name, u8 mode, for `super.name` in a method. In mode 0, a call, [self][nil] becomes
[self][the parent's method]. In mode 1, a property, [self] becomes the value of the parent's
property. The search starts at the parent of the class the code is written in, not at the
class of self. */
    LZ_OP_CLASS_LINK,  /* u16 constant for the class, u16 constant for the parent's name, u8 role. Pops the parent class
or trait named in the head `class A : B, C` and joins it to the class. Role 0 is the first
entry and 1 is a later one */
    LZ_OP_ERROR_CLASS, /* u16 constant for the class, u16 constant for the name of the kind, u8 reserved. Binds the body
of `error Name : Parent { ... }` to its kind */
    LZ_OP_EXTEND,      /* u16 constant for an extend block, which is a hidden class, u16 constant for the name of the
target, u8 mode. Registers the block. Mode 0 is a builtin kind and the block says which, and
mode 1 means the target is the class that the global of that name holds */
    LZ_OP_IMPORT       /* u16 constant for the module id, u16 constant for the bound name, u16 constant for the
name in the module, u8 mode. Runs the module once and copies its `pub` values into this module's globals.
Mode 0 is `import m`: every export is stored as "bound.name". Mode 1 is `from m import a as b`: the export
`a` is stored as `b` */
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
    uint32_t *type_slots; /* open-addressing index into types, 0 is empty and anything else is index + 1 */
    size_t type_slot_capacity;
    const struct LzModule *module; /* the module this chunk's code belongs to, borrowed, or NULL for main */
    struct LzInterface *iface; /* what the unit's `pub` declarations offer, see lizard/interface.h, or NULL for a function's chunk */
} LzChunk;

void lz_chunk_init(LzChunk *chunk);

/* Releases the references held by the constant table. */
void lz_chunk_free(LzHeap *heap, LzChunk *chunk);

/* Frees the arrays without releasing the constants, for a heap that frees every object itself. */
void lz_chunk_discard(LzChunk *chunk);

bool lz_chunk_write(LzChunk *chunk, uint8_t byte, uint32_t source_offset);
bool lz_chunk_write_u16(LzChunk *chunk, uint16_t operand, uint32_t source_offset);

/* The chunk retains the value. Fails when out of memory or past LZ_CONSTANTS_MAX. */
bool lz_chunk_add_constant(LzChunk *chunk, LzValue value, uint16_t *index);

/* Stores a type once, and an identical type already in the table is reused. */
bool lz_chunk_add_type(LzChunk *chunk, const LzType *type, uint16_t *index);

const char *lz_opcode_name(uint8_t opcode);

/* Total size of an instruction in bytes, or 0 for an unknown opcode. */
size_t lz_opcode_length(uint8_t opcode);

/* Drops code and constants added after the given marks, releasing those constants. */
void lz_chunk_truncate(LzHeap *heap, LzChunk *chunk, size_t code_count, size_t constant_count);

#endif
