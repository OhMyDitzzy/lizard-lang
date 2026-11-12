#ifndef LIZARD_COMPILER_H
#define LIZARD_COMPILER_H

#include "lizard/chunk.h"
#include "lizard/source.h"
#include "lizard/suggest.h"
#include "lizard/vm.h"

#define LZ_MAX_DIAGNOSTICS 16
#define LZ_MAX_NAME_HINTS 64

/*
 * What an error says beyond its message, which is text for the label, a note and a help, and
 * optionally a visual fix (see LzFix in report.h). The fix deletes `fix_delete` bytes at
 * `fix_offset` and inserts `fix_text`, or puts `fix_text` on a new line before the line of
 * `fix_offset`.
 */
typedef struct LzDiagExtras {
    char label[160]; /* when not empty, replaces LzDiagnostic.label */
    char note[200];
    char help[200]; /* a "= help:" line, and the fallback when the fix can't be drawn */
    bool has_fix;
    bool keep_help; /* show the help line next to the fix too, instead of only as its fallback */
    bool fix_insert_line;
    bool fix_quiet;
    bool fix_has_indent;
    uint32_t fix_offset;
    uint32_t fix_delete;
    char fix_headline[96];
    char fix_text[128];
    char fix_indent[40];
} LzDiagExtras;

typedef struct LzDiagnostic {
    uint32_t offset;
    uint32_t length;
    char message[256]; /* as long as vm->error, so nothing is cut */
    const char *label; /* short text under the carets, a static string, or NULL */
    LzSuggestions suggestions; /* "did you mean" names close to the one used, if any */
    LzDiagExtras extras;
} LzDiagnostic;

/*
 * A use of a name that doesn't exist yet. That's not a compile error because the script runs
 * statement by statement, but the compiler is the one that knows which names are in scope. So it
 * works out the "did you mean" candidates now and keeps them here for the run-time error that
 * follows if the statement really runs. Look one up with lz_diagnostics_find_hint using
 * vm->error_offset.
 */
typedef struct LzNameHint {
    uint32_t offset;
    uint32_t length;
    bool is_call;  /* written as name(...), so the candidates are functions */
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
 * Compiles a whole script into chunk in a single pass. Errors are collected, several per run,
 * recovering at statement boundaries, and the call returns LZ_ERR_COMPILE. LZ_ERR_MEMORY or
 * LZ_ERR_HEAP_LIMIT mean the compiler itself ran out of memory and vm->error says why. On any
 * failure the chunk is unusable but must still be released with lz_chunk_free.
 *
 * Builtins and globals already defined in vm are visible to the script, so install them first.
 * Constants are evaluated at compile time with the VM's own arithmetic.
 */
LzStatus lz_compile(LzVM *vm, const LzSource *source, LzChunk *chunk, LzDiagnostics *diagnostics);

/*
 * Why a module failed to compile, kept for the host and replaced by the next failure. The source
 * is the module's own text. See lz_vm_module_failure.
 */
typedef struct LzModuleFailure {
    LzSource source;
    LzDiagnostics diagnostics;
    struct LzModuleFailure *next; /* the failure before this one: a module that this one imports */
    char *id;                     /* the module's id, so a module that failed is reported once per compile */
    bool cycle;                   /* it failed only because an import led back into a cycle, which is said once */
} LzModuleFailure;

/*
 * The full diagnostics of the last module that failed to compile, or NULL. When it failed because a
 * module it imports failed too, `next` leads to that one, and so on down to the first. Owned by the
 * VM, and replaced when a new compile starts.
 */
const LzModuleFailure *lz_vm_module_failure(const LzVM *vm);

/* The reports of lz_diagnostics_report without the closing "aborting" line, for a caller that writes several. */
void lz_diagnostics_report_body(const LzDiagnostics *diagnostics, const LzSource *source, bool color,
                                const LzWriter *out);

/*
 * Writes the reports of every module that failed to compile, the one that failed first first, each
 * against its own source. Returns how many errors it wrote.
 */
size_t lz_vm_module_failures_report(const LzVM *vm, bool color, const LzWriter *out);

/*
 * Like lz_compile, but the script is the code of `module`: its globals are the module's table,
 * and every chunk it makes has chunk.module set. The module owns `source`'s text for as long as
 * the VM lives, because chunks and functions point at offsets in it and reports need it.
 */
LzStatus lz_compile_module(LzVM *vm, LzModule *module, const LzSource *source, LzChunk *chunk,
                           LzDiagnostics *diagnostics);

/* Prints every collected error with its source line, then a count of dropped ones. */
void lz_diagnostics_write(const LzDiagnostics *diagnostics, const LzSource *source,
                          const LzWriter *out);

/*
 * The same errors as full reports, see report.h. They have labels, "did you mean" snippets, an
 * optional colour, and a closing "aborting due to N previous errors".
 */
void lz_diagnostics_report(const LzDiagnostics *diagnostics, const LzSource *source, bool color,
                           const LzWriter *out);

/* The hint recorded for an undefined name used at this source offset, or NULL. */
const LzNameHint *lz_diagnostics_find_hint(const LzDiagnostics *diagnostics, uint32_t offset);

#endif
