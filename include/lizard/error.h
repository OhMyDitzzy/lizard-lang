#ifndef LIZARD_ERROR_H
#define LIZARD_ERROR_H

#include "lizard/common.h"
#include "lizard/memory.h"
#include "lizard/value.h"
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

/* The most kinds there can be: the built-in ones and those a script declares with `error`. */
#define LZ_ERROR_TYPES_MAX 256

#include "lizard/array.h"


/* The built-in kinds only: a declared one is known to a VM, see lz_vm_error_kind_name. */
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

/*
 * An error value. `kind` is an LzErrorKind for the built-in kinds and an index from
 * LZ_ERROR_KIND_COUNT up for the kinds a script declares. `cause` is an error or nil and `data`
 * any value; both are owned by the error. A container held as `data` counts the error as one of
 * its holders, so the cycle check of arrays and dicts can see through errors.
 */
typedef struct LzError {
    LzObject base;
    uint8_t kind;
    LzString *kind_name; /* the interned name of the kind (retained), so an error can write itself */
    LzString *message;
    LzArray *trace; /* strings, innermost first; may be NULL */
    LzValue cause;
    LzValue data;
    uint64_t is_kind[4];        /* bit k: the kind of this error is kind k or a descendant of it */
    struct LzInstance *payload; /* an error of a class declared with `error`: the instance that holds its fields (retained), or NULL */
} LzError;

static inline bool lz_value_is_error(const LzValue *value) {
    return value->type == LZ_VAL_OBJECT && value->as.object->type == LZ_OBJ_ERROR;
}

struct LzVM;
struct LzNative;

/*
 * The kinds of one VM: the built-in ones, then the declared ones in the order of their
 * declarations. A declared kind has a parent that was known before it, so the chain of
 * parents never loops.
 */
const char *lz_vm_error_kind_name(const struct LzVM *vm, unsigned kind);
bool lz_vm_error_kind_from_name(const struct LzVM *vm, const char *name, size_t length,
                                uint8_t *out);
bool lz_vm_error_kind_parent(const struct LzVM *vm, unsigned kind, uint8_t *parent);
bool lz_vm_error_kind_is(const struct LzVM *vm, unsigned kind, unsigned ancestor);
size_t lz_vm_error_kind_count(const struct LzVM *vm);

/*
 * Declares a kind (`error Name : Parent`) and defines the function that makes errors of it in
 * the builtin scope. `name` must be interned. LZ_ERR_RUNTIME with vm->error set when the limit
 * of kinds is reached.
 */
LzStatus lz_vm_error_declare(struct LzVM *vm, LzString *name, uint8_t parent, uint8_t *out);

/* Is this native the function that makes errors of a kind? *kind says which one. */
bool lz_native_error_kind(const struct LzNative *native, uint8_t *kind);

/* Makes the plain error of a constructor call (checks the types); the creation reference moves to *result. */
LzStatus lz_error_construct_core(struct LzVM *vm, uint8_t kind, const LzValue *message,
                                 const LzValue *cause, const LzValue *data, LzValue *result);

/* The class bound to a declared kind (`error Name : Parent { ... }`), or NULL. */
struct LzClass *lz_vm_error_class(const struct LzVM *vm, unsigned kind);
/* Binds a class to a declared kind (retained); widens the constructor to take the fields too. */
LzStatus lz_vm_error_set_class(struct LzVM *vm, uint8_t kind, struct LzClass *cls);
/* Lets the constructor of the kind take the fields of a class declared in the file (before it runs). */
void lz_vm_error_widen_constructor(struct LzVM *vm, uint8_t kind);
void lz_vm_release_error_classes(struct LzVM *vm);

/* The native behind every error constructor; the kind is in the native's data. */
LzStatus lz_error_type_native(struct LzVM *vm, int argc, const LzValue *argv, LzValue *result);

/*
 * Installs the function that makes errors of `kind` under `name`; the built-in kinds and the
 * declared ones share it. `name` must be interned.
 */
LzStatus lz_vm_error_define_constructor(struct LzVM *vm, LzString *name, uint8_t kind);

/*
 * `Kind(message, cause: ..., data: ...)`: the call with named arguments. argv are the
 * `positional` arguments by position; names holds the `named_count` names separated by NUL
 * bytes and `named` their values. Borrowed: whatever the error keeps it retains.
 */
LzStatus lz_error_construct_named(struct LzVM *vm, uint8_t kind, size_t positional,
                                  const LzValue *argv, size_t named_count, const char *names,
                                  size_t names_length, const LzValue *named, LzValue *result);

/*
 * Makes an error value (the caller owns the reference). The message is copied. On failure the
 * status says why (the heap limit or memory).
 */
LzStatus lz_error_new(struct LzVM *vm, unsigned kind, const char *message, size_t length,
                      LzError **out);

/*
 * Gives `error` a trace, the one vm->trace holds (innermost first, one string per frame, in the
 * form "name (file:line:column)"), unless it has one already: a thrown error that is thrown
 * again keeps the trace of where it first was.
 */
LzStatus lz_error_attach_trace(struct LzVM *vm, LzError *error);

/*
 * Gives the error a cause (an error or nil) and data (any value). Both are retained. Nothing can
 * form a cycle here: the error is new, so nothing holds it yet. The caller checks the types.
 */
void lz_error_set_extra(struct LzVM *vm, LzError *error, const LzValue *cause, const LzValue *data);

/* The text of an error for println and friends: the to_string / to_debug of its class when it has one,
 * else "Kind: message" (debug form of an error with fields: Kind("message", field: v)). */
bool lz_error_write_text(const LzError *error, const LzWriter *writer, bool debug);

/* "Kind: message". Returns false when the writer fails. */
bool lz_error_write(const LzError *error, const LzWriter *writer);

#endif
