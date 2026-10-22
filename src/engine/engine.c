#include "lizard/engine.h"

#include <string.h>

#include "ge_internal.h"

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

    GeRun run = {&program, 0, 0, 0, 0};
    uint64_t before = vm->instructions;
    LzStatus status = lz_vm_eval_with(vm, chunk, result, ge_execute, &run);
    fill_stats(stats, vm->instructions - before, run.dispatches, run.delegated, &program);
    if (stats != NULL) {
        stats->program_size += run.function_ops;
        stats->fused_sites += run.function_fused;
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
