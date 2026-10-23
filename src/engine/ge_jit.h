#ifndef LIZARD_GE_JIT_H
#define LIZARD_GE_JIT_H

#include "ge_internal.h"

/*
 * The Gecko JIT: a baseline compiler from the GE IR to x86-64 machine code.
 * See docs/ENGINE.md ("The JIT") for the design. In short: the native code works on
 * the very same value stack and frame stack as the interpreter, so at any
 * instruction boundary it can hand control back ("bail out") and the interpreter
 * simply continues from that instruction. Every fast path checks exactly what it
 * assumes before changing anything, as in ge_exec.c.
 */

/*
 * Which backends this build has. LZ_NO_JIT (CMake: -DLZ_ENABLE_JIT=OFF) turns them all off.
 */
#if !defined(LZ_NO_JIT) && defined(__x86_64__) && (defined(__linux__) || defined(__APPLE__))
#define GE_JIT_X64 1
#else
#define GE_JIT_X64 0
#endif
#if !defined(LZ_NO_JIT) && defined(__aarch64__) && (defined(__linux__) || defined(__APPLE__))
#define GE_JIT_A64 1
#else
#define GE_JIT_A64 0
#endif
#define GE_JIT_HAVE_BACKEND (GE_JIT_X64 || GE_JIT_A64)

/* One inline cache for a global-variable access. */
typedef struct JitSite {
    uint32_t hint;  /* slot in the globals table */
    uint32_t bhint; /* slot in the builtins table */
    uint32_t count; /* size of the globals table when the builtin was cached */
} JitSite;

enum { GE_JX_BAIL = 1, GE_JX_FAIL, GE_JX_HALT, GE_JX_RETURNED, GE_JX_ENTER };

/* How native code leaves: written by the exit stubs, read by the interpreter. */
typedef struct GeJitExit {
    uint64_t saved_rsp; /* must stay first: the trampoline addresses it directly */
    uint32_t reason;    /* GE_JX_* */
    uint32_t pc;        /* instruction index to continue at (BAIL) or that failed (FAIL) */
    uint32_t status;    /* LzStatus (FAIL) */
} GeJitExit;

struct GeJit {
    void *mem;
    size_t mem_size;
    size_t code_size;
    void **entry;    /* count entries: native address of each instruction that may be entered, or NULL */
    int32_t *depth;  /* count entries: stack height above the frame base at each instruction */
    size_t count;
    uint32_t max_depth;
    void *entry_call; /* a call from native code: prologue, stack check, then instruction 0 */
    JitSite *sites;
};

/* True when this build and platform can run generated code. */
bool ge_jit_supported(void);

/*
 * Compiles a translated program. entry_depth is the stack height at instruction 0:
 * the parameter slots for a function, 0 for the script. NULL when the program is not
 * of a shape the JIT handles (the interpreter then runs it, as always) or on failure.
 */
struct GeJit *ge_jit_compile(const LzChunk *chunk, const GeProgram *program, size_t entry_depth);
void ge_jit_free(struct GeJit *jit);

/*
 * Runs native code from instruction pc of a frame whose slot 0 is at stack index
 * base and whose stack holds sp - base values. False, with nothing done, when the
 * code cannot be entered there. Fuel must be unlimited.
 */
bool ge_jit_enter(LzVM *vm, const struct GeJit *jit, size_t pc, size_t base, size_t sp,
                  GeJitExit *exit);

#endif
