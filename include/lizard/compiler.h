#ifndef LIZARD_COMPILER_H
#define LIZARD_COMPILER_H

#include "lizard/chunk.h"
#include "lizard/source.h"
#include "lizard/vm.h"

#define LZ_MAX_DIAGNOSTICS 16

typedef struct LzDiagnostic {
    uint32_t offset;
    uint32_t length;
    char message[256]; /* as long as vm->error, so nothing is cut */
} LzDiagnostic;

typedef struct LzDiagnostics {
    LzDiagnostic items[LZ_MAX_DIAGNOSTICS];
    size_t count;
    size_t dropped; /* errors past LZ_MAX_DIAGNOSTICS */
} LzDiagnostics;

/*
 * Compiles a whole script into chunk in a single pass. Errors are collected
 * (several per run, recovering at statement boundaries) and the call returns
 * LZ_ERR_COMPILE. LZ_ERR_MEMORY or LZ_ERR_HEAP_LIMIT mean the compiler itself
 * ran out of memory and vm->error says why. On any failure the chunk is
 * unusable but must still be released with lz_chunk_free.
 *
 * Builtins and globals already defined in vm are visible to the script, so
 * install them first. Constants are evaluated at compile time with the VM's
 * own arithmetic.
 */
LzStatus lz_compile(LzVM *vm, const LzSource *source, LzChunk *chunk, LzDiagnostics *diagnostics);

/* Prints every collected error with its source line, then a count of dropped ones. */
void lz_diagnostics_write(const LzDiagnostics *diagnostics, const LzSource *source,
                          const LzWriter *out);

#endif
