#include "lizard/engine.h"

#include <stdlib.h>
#include <string.h>

#include "ge_internal.h"
#include "ge_jit.h"

const char *lz_engine_name(LzEngine engine) {
    switch (engine) {
    case LZ_ENGINE_REFERENCE:
        return "ref";
    case LZ_ENGINE_GE:
        return "ge";
    }
    return "?";
}

bool lz_engine_from_name(const char *name, LzEngine *out) {
    if (strcmp(name, "ref") == 0 || strcmp(name, "reference") == 0) {
        *out = LZ_ENGINE_REFERENCE;
        return true;
    }
    if (strcmp(name, "ge") == 0 || strcmp(name, "gecko") == 0) {
        *out = LZ_ENGINE_GE;
        return true;
    }
    return false;
}

#define GE_JIT_THRESHOLD 50u

static int g_jit_mode = -1; /* -1: not decided yet */

bool lz_jit_mode_from_name(const char *name, LzJitMode *out) {
    if (strcmp(name, "off") == 0 || strcmp(name, "0") == 0) {
        *out = LZ_JIT_OFF;
    } else if (strcmp(name, "on") == 0 || strcmp(name, "1") == 0) {
        *out = LZ_JIT_ON;
    } else if (strcmp(name, "eager") == 0) {
        *out = LZ_JIT_EAGER;
    } else {
        return false;
    }
    return true;
}

void lz_engine_set_jit(LzJitMode mode) {
    g_jit_mode = (int)mode;
}

LzJitMode lz_engine_jit_mode(void) {
    if (g_jit_mode < 0) {
        LzJitMode mode = LZ_JIT_ON;
        const char *env = getenv("LZ_JIT");
        if (env != NULL) {
            (void)lz_jit_mode_from_name(env, &mode);
        }
        g_jit_mode = (int)mode;
    }
    return (LzJitMode)g_jit_mode;
}

bool lz_engine_jit_available(void) {
    return ge_jit_supported();
}

static void fill_stats(LzEngineStats *stats, uint64_t instructions, uint64_t dispatches,
                       uint64_t delegated, const GeProgram *program) {
    if (stats == NULL) {
        return;
    }
    stats->instructions = instructions;
    stats->dispatches = dispatches;
    stats->delegated = delegated;
    stats->program_size = program != NULL ? program->count : 0;
    stats->fused_sites = program != NULL ? program->fused_sites : 0;
    stats->translated = program != NULL;
    stats->jit_enters = 0;
    stats->jit_programs = 0;
    stats->jit_bytes = 0;
}

static LzStatus eval_reference(LzVM *vm, const LzChunk *chunk, LzValue *result,
                               LzEngineStats *stats) {
    uint64_t before = vm->instructions;
    LzStatus status = lz_vm_eval(vm, chunk, result);
    uint64_t executed = vm->instructions - before;
    fill_stats(stats, executed, executed, 0, NULL);
    return status;
}

LzStatus lz_ge_eval_stats(LzVM *vm, const LzChunk *chunk, LzValue *result, LzEngineStats *stats) {
    GeProgram program;
    if (!ge_translate(chunk, &program)) {
        return eval_reference(vm, chunk, result, stats);
    }

    GeRun run = {&program, 0, 0, 0, 0, false, false, GE_JIT_THRESHOLD, 0, 0, 0};
    LzJitMode mode = lz_engine_jit_mode();
    if (mode != LZ_JIT_OFF && ge_jit_supported()) {
        run.jit = true;
        run.jit_eager = mode == LZ_JIT_EAGER;
    }
    uint64_t before = vm->instructions;
    LzStatus status = lz_vm_eval_with(vm, chunk, result, ge_execute, &run);
    fill_stats(stats, vm->instructions - before, run.dispatches, run.delegated, &program);
    if (stats != NULL) {
        stats->program_size += run.function_ops;
        stats->fused_sites += run.function_fused;
        stats->jit_enters = run.jit_enters;
        stats->jit_programs = run.jit_programs;
        stats->jit_bytes = run.jit_bytes;
    }
    ge_program_free(&program);
    return status;
}

LzStatus lz_ge_eval(LzVM *vm, const LzChunk *chunk, LzValue *result) {
    return lz_ge_eval_stats(vm, chunk, result, NULL);
}

LzStatus lz_ge_run(LzVM *vm, const LzChunk *chunk) {
    return lz_ge_eval_stats(vm, chunk, NULL, NULL);
}

LzStatus lz_engine_eval(LzEngine engine, LzVM *vm, const LzChunk *chunk, LzValue *result,
                        LzEngineStats *stats) {
    if (engine == LZ_ENGINE_GE) {
        return lz_ge_eval_stats(vm, chunk, result, stats);
    }
    return eval_reference(vm, chunk, result, stats);
}

LzStatus lz_engine_run(LzEngine engine, LzVM *vm, const LzChunk *chunk, LzEngineStats *stats) {
    return lz_engine_eval(engine, vm, chunk, NULL, stats);
}
