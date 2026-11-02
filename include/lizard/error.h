#ifndef LIZARD_ERROR_H
#define LIZARD_ERROR_H

#include "lizard/common.h"
#include "lizard/memory.h"
#include "lizard/value.h"
#include "lizard/array.h"
#include "lizard/writer.h"

/*
 * The built-in kinds of error. One inheritance chain per kind, only for errors:
 *
 *   Error
 *   +- RuntimeError
 *   |  +- TypeError, ValueError, RangeError, ArithmeticError, ArityError
 *   +- IOError
 */
typedef enum LzErrorKind {
    LZ_ERROR_ERROR = 0,
    LZ_ERROR_RUNTIME,
    LZ_ERROR_TYPE,
    LZ_ERROR_VALUE,
    LZ_ERROR_RANGE,
    LZ_ERROR_ARITHMETIC,
    LZ_ERROR_ARITY,
    LZ_ERROR_IO,
    LZ_ERROR_KIND_COUNT
} LzErrorKind;

const char *lz_error_kind_name(LzErrorKind kind);
bool lz_error_kind_from_name(const char *name, size_t length, LzErrorKind *out);

/* The parent of a kind; Error has none (returns false). */
bool lz_error_kind_parent(LzErrorKind kind, LzErrorKind *parent);

/* Is `kind` the same as `ancestor` or one of its descendants? */
bool lz_error_kind_is(LzErrorKind kind, LzErrorKind ancestor);

/*
 * The kind that fits a run-time failure: the status says it when it can (an arity or I/O failure),
 * the wording of the message says the rest. Everything else is a RuntimeError.
 */
LzErrorKind lz_error_classify(LzStatus status, const char *message);

/*
 * Can a script catch this failure? The budgets of the sandbox (instructions, heap, memory, stack
 * depth) cannot be caught: they stop the program, and no catch or finally block runs.
 */
bool lz_status_catchable(LzStatus status);

/* An error value. `cause` and `data` are kept for later; they are nil for now. */
typedef struct LzError {
    LzObject base;
    uint8_t kind; /* an LzErrorKind */
    LzString *message;
    LzArray *trace; /* strings, innermost first; may be NULL */
    LzValue cause;
    LzValue data;
} LzError;

static inline bool lz_value_is_error(const LzValue *value) {
    return value->type == LZ_VAL_OBJECT && value->as.object->type == LZ_OBJ_ERROR;
}

struct LzVM;

/*
 * Makes an error value (the caller owns the reference). The message is copied. On failure the
 * status says why (the heap limit or memory).
 */
LzStatus lz_error_new(struct LzVM *vm, LzErrorKind kind, const char *message, size_t length,
                      LzError **out);

/*
 * Gives `error` a trace, the one vm->trace holds (innermost first, one string per frame, in the
 * form "name (file:line:column)"), unless it has one already: a thrown error that is thrown
 * again keeps the trace of where it first was.
 */
LzStatus lz_error_attach_trace(struct LzVM *vm, LzError *error);

/* "Kind: message". Returns false when the writer fails. */
bool lz_error_write(const LzError *error, const LzWriter *writer);

#endif
