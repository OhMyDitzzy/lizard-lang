/*
 * The Gecko JIT on platforms without a backend (or with LZ_NO_JIT): nothing is compiled
 * and every program runs on the GE interpreter. The backends are ge_jit_x64.c and
 * ge_jit_a64.c; ge_jit_rt.c holds what they share.
 */

#include "ge_jit.h"

#if !GE_JIT_HAVE_BACKEND

bool ge_jit_supported(void) {
    return false;
}

struct GeJit *ge_jit_compile(const LzChunk *chunk, const GeProgram *program, size_t entry_depth) {
    (void)chunk;
    (void)program;
    (void)entry_depth;
    return NULL;
}

bool ge_jit_enter(LzVM *vm, const struct GeJit *jit, size_t pc, size_t base, size_t sp,
                  GeJitExit *exit) {
    (void)vm;
    (void)jit;
    (void)pc;
    (void)base;
    (void)sp;
    (void)exit;
    return false;
}

#endif
