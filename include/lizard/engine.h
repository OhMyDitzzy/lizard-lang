#ifndef LIZARD_ENGINE_H
#define LIZARD_ENGINE_H

#include "lizard/vm.h"

/*
 * Execution engines.
 *
 * Every engine runs the same bytecode with the same observable behaviour: the
 * same result, output, error message and source position, the same fuel spent
 * and the same vm->instructions. They differ only in speed.
 */
typedef enum LzEngine {
    LZ_ENGINE_REFERENCE, /* core/run.c: the plain interpreter, the definition of the semantics */
    LZ_ENGINE_GE         /* Gecko Engine: pre-decoded, fused, inline-cached (docs/ENGINE.md) */
} LzEngine;

/* What an engine did for one run. Filled in on request; every field is valid on every engine. */
typedef struct LzEngineStats {
    uint64_t instructions; /* bytecode instructions executed (identical on every engine) */
    uint64_t dispatches;   /* dispatch-loop iterations; fewer than instructions when GE fuses */
    uint64_t delegated;    /* instructions GE handed to the reference interpreter */
    size_t program_size;   /* GE instructions after translation, 0 when not translated */
    size_t fused_sites;    /* sites where several instructions became one */
    bool translated;       /* false on the reference engine, or when GE fell back to it */
} LzEngineStats;

const char *lz_engine_name(LzEngine engine);

/* Accepts "ref" (or "reference") and "ge" (or "gecko"). */
bool lz_engine_from_name(const char *name, LzEngine *out);

/* Same contracts as lz_vm_eval / lz_vm_run; stats may be NULL. */
LzStatus lz_engine_eval(LzEngine engine, LzVM *vm, const LzChunk *chunk, LzValue *result,
                        LzEngineStats *stats);
LzStatus lz_engine_run(LzEngine engine, LzVM *vm, const LzChunk *chunk, LzEngineStats *stats);

/*
 * Gecko Engine entry points. A chunk that GE cannot translate (truncated
 * instructions, jumps into the middle of an instruction) runs on the
 * reference interpreter, so these never behave differently from lz_vm_eval.
 */
LzStatus lz_ge_eval(LzVM *vm, const LzChunk *chunk, LzValue *result);
LzStatus lz_ge_run(LzVM *vm, const LzChunk *chunk);
LzStatus lz_ge_eval_stats(LzVM *vm, const LzChunk *chunk, LzValue *result, LzEngineStats *stats);

#endif
