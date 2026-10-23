#ifndef LIZARD_GE_INTERNAL_H
#define LIZARD_GE_INTERNAL_H

#include "lizard/engine.h"

/*
 * The Gecko Engine IR: one GeIns per bytecode instruction, in the same order,
 * with operands decoded and jump targets resolved to instruction indexes.
 * Because the mapping is 1:1, a fused instruction can always fall back to the
 * plain instruction at the same index, and the instructions it covers are still
 * there for the fallback to continue through.
 */

/* Where a fused operand comes from. */
enum { GE_K_LOCAL = 0, GE_K_GLOBAL = 1, GE_K_CONST = 2 };

/* Where a fused result goes, besides the stack. */
enum { GE_ST_NONE = 0, GE_ST_LOCAL = 1, GE_ST_GLOBAL = 2 };

/* Values below 64 are LzOpCode: a plain instruction that GE runs itself. */
enum {
    GE_OP_END = 64, /* sentinel after the last instruction: running off the chunk */
    GE_OP_DELEGATE, /* a plain instruction GE does not specialise: reference interpreter */
    GE_OP_BIN,      /* [load a] [load b] ARITH [store]          */
    GE_OP_BIN1,     /*          [load b] ARITH [store]   (left operand on the stack) */
    GE_OP_CMPJ,     /* [load a] [load b] CMP JUMP_IF_FALSE POP */
    GE_OP_CMPJ1,    /*          [load b] CMP JUMP_IF_FALSE POP  (left operand on the stack) */
    GE_OP_INDEX,    /* [load a] [load b] INDEX                  */
    GE_OP_INDEX1    /*          [load b] INDEX                  (array on the stack) */
};

#define GE_NO_HINT UINT32_MAX

typedef struct GeIns {
    uint8_t op;      /* LzOpCode or GE_OP_* */
    uint8_t plain;   /* the original LzOpCode of the instruction at this index */
    uint8_t sub;     /* fused: the arithmetic or comparison opcode */
    uint8_t kinds;   /* fused: operand kinds, 2 bits each: a, b, store */
    uint16_t a;      /* plain: the u16 operand. fused: first operand (slot or constant index) */
    uint16_t b;      /* fused: second operand */
    uint16_t c;      /* fused: store target (slot or name constant index) */
    uint16_t span;   /* bytecode instructions this one stands for */
    uint32_t target; /* jump instructions: index to continue at */
    uint32_t offset; /* byte offset of the first covered instruction in the chunk */
    uint32_t hint_a; /* inline caches: slot in the globals table last seen for each operand */
    uint32_t hint_b;
    uint32_t hint_c;
} GeIns;

struct GeJit;

/* Where a program stands with the JIT (ge_jit.h). */
enum { GE_JIT_UNTRIED = 0, GE_JIT_COMPILED = 1, GE_JIT_FAILED = 2, GE_JIT_DISABLED = 3 };

typedef struct GeProgram {
    GeIns *ins;         /* count entries; ins[count - 1] is the GE_OP_END sentinel */
    size_t count;
    size_t fused_sites;
    struct GeJit *jit;         /* native code for this program, or NULL */
    struct GeJit *jit_retired; /* code given up on after too many bail-outs (freed with the program) */
    uint32_t hot;              /* loop back-edges and calls seen by the interpreter */
    uint32_t jit_enters;       /* times the interpreter entered the native code */
    uint32_t jit_bails;        /* times the native code handed an instruction back */
    uint8_t jit_state;
} GeProgram;

typedef struct GeRun {
    GeProgram *program;
    uint64_t dispatches;
    uint64_t delegated;
    size_t function_ops;   /* GE instructions of the function programs translated during the run */
    size_t function_fused; /* fused sites in them */
    bool jit;              /* may this run use native code? */
    bool jit_eager;        /* compile every program on first sight (tests) */
    uint32_t jit_threshold;
    uint64_t jit_enters;   /* entries into native code */
    size_t jit_programs;   /* programs compiled during the run */
    size_t jit_bytes;      /* machine code generated */
} GeRun;

/*
 * Translates a chunk. False when the chunk cannot be translated faithfully
 * (an unknown opcode, an instruction cut off by the end of the chunk, or a jump
 * that does not land on an instruction boundary) or on out of memory; the
 * caller then runs the reference interpreter, which defines what such code does.
 */
bool ge_translate(const LzChunk *chunk, GeProgram *program);
void ge_program_free(GeProgram *program);

/* Releases a program cached on a function object (LzFunction.engine_cache_free). */
void ge_cache_free(void *cache);

/* An LzExecuteFn: context is a GeRun. */
LzStatus ge_execute(LzVM *vm, const LzChunk *chunk, size_t *failed_at, void *context);

#endif
