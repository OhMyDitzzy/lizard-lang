#ifndef LIZARD_COMPILER_H
#define LIZARD_COMPILER_H

#include "lizard/chunk.h"
#include "lizard/source.h"
#include "lizard/suggest.h"
#include "lizard/vm.h"

#define LZ_MAX_DIAGNOSTICS 16
#define LZ_MAX_NAME_HINTS 64

typedef struct LzDiagnostic {
    uint32_t offset;
    uint32_t length;
    char message[256]; /* as long as vm->error, so nothing is cut */
    const char *label; /* short text under the carets (static string), or NULL */
    LzSuggestions suggestions; /* "did you mean": names close to the one used, if any */
} LzDiagnostic;

/*
 * A use of a name that does not exist yet. That is not a compile error (the script
 * runs statement by statement), but the compiler is the one that knows which names
 * are in scope, so it works out the "did you mean" candidates now and keeps them here
 * for the run-time error that follows if the statement really runs. Look one up with
 * lz_diagnostics_find_hint using vm->error_offset.
 */
typedef struct LzNameHint {
    uint32_t offset;
    uint32_t length;
    bool is_call;  /* written as name(...): the candidates are functions */
    bool is_assignment;
    bool is_method; /* written receiver.name(...) */
    LzSuggestions suggestions;
} LzNameHint;

typedef struct LzDiagnostics {
    LzDiagnostic items[LZ_MAX_DIAGNOSTICS];
    size_t count;
    size_t dropped; /* errors past LZ_MAX_DIAGNOSTICS */
    LzNameHint hints[LZ_MAX_NAME_HINTS];
    size_t hint_count;
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

/*
 * The same errors as full reports (see report.h): labels, "did you mean" snippets, an
 * optional colour, and a closing "aborting due to N previous errors".
 */
void lz_diagnostics_report(const LzDiagnostics *diagnostics, const LzSource *source, bool color,
                           const LzWriter *out);

/* The hint recorded for an undefined name used at this source offset, or NULL. */
const LzNameHint *lz_diagnostics_find_hint(const LzDiagnostics *diagnostics, uint32_t offset);

#endif
