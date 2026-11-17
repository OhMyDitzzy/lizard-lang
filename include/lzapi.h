#ifndef LZAPI_H
#define LZAPI_H

/*
 * The public API for native modules of Lizard. This is the only header a module author includes. It
 * depends on nothing but the C standard headers and shows no internal layout.
 *
 * Every function here must be called from the thread that runs the VM. Native code is trusted: it
 * runs with the full rights of the process and nothing here is a sandbox. See docs/NATIVE.md.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LZN_ABI_VERSION 1

#if defined(__GNUC__) || defined(__clang__)
#define LZN_PRINTF(fmt, first) __attribute__((format(printf, fmt, first)))
#else
#define LZN_PRINTF(fmt, first)
#endif

/* Opaque types. */
typedef struct LznCtx LznCtx;         /* one native call, or the init of a module */
typedef struct LznValue LznValue;     /* a handle, valid until the native function returns */
typedef struct LznRef LznRef;         /* a strong reference that outlives the call */
typedef struct LznBuilder LznBuilder; /* declares a module while its init runs */

typedef enum LznStatus {
    LZN_OK = 0,
    LZN_FAIL = 1 /* an error is set, or the value was of the wrong kind. Return it as it is */
} LznStatus;

typedef enum LznKind {
    LZN_NIL,
    LZN_BOOL,
    LZN_INT,
    LZN_FLOAT,
    LZN_STRING,
    LZN_ARRAY,
    LZN_DICT,
    LZN_FUNCTION,
    LZN_INSTANCE,
    LZN_LINK,
    LZN_ERROR,
    LZN_COROUTINE
} LznKind;

/* The built-in kinds of error. A module's own kinds come from lzn_module_error. */
typedef enum LznErrKind {
    LZN_ERR_ERROR = 0,
    LZN_ERR_RUNTIME,
    LZN_ERR_TYPE,
    LZN_ERR_VALUE,
    LZN_ERR_RANGE,
    LZN_ERR_ARITHMETIC,
    LZN_ERR_ARITY,
    LZN_ERR_IO
} LznErrKind;

/*
 * An error kind: a built-in one, or one a module declared. A module's kind is a small number that
 * lzn_module_error returned and is good only inside that module.
 */
typedef int LznErrId;
#define LZN_ERR_FIRST_MODULE 256

typedef LznStatus (*LznFn)(LznCtx *ctx);
typedef LznStatus (*LznInitFn)(LznBuilder *b);


/* For a method, argument 0 is the first one after self. */
int lzn_arg_count(LznCtx *ctx);
LznValue *lzn_arg(LznCtx *ctx, int index); /* NULL when out of range */

LznValue *lzn_nil(LznCtx *ctx);
LznValue *lzn_bool(LznCtx *ctx, bool value);
LznValue *lzn_int(LznCtx *ctx, int64_t value);
LznValue *lzn_float(LznCtx *ctx, double value);
/* Copies the bytes. NULL with an error set when out of memory or over the heap limit. */
LznValue *lzn_string(LznCtx *ctx, const char *data, size_t length);
LznValue *lzn_array(LznCtx *ctx);
LznValue *lzn_dict(LznCtx *ctx);

LznStatus lzn_return(LznCtx *ctx, LznValue *value);
LznStatus lzn_return_nil(LznCtx *ctx);
LznStatus lzn_return_bool(LznCtx *ctx, bool value);
LznStatus lzn_return_int(LznCtx *ctx, int64_t value);
LznStatus lzn_return_float(LznCtx *ctx, double value);
LznStatus lzn_return_string(LznCtx *ctx, const char *data, size_t length);


LznKind lzn_kind(LznCtx *ctx, LznValue *value);
const char *lzn_kind_name(LznKind kind);

/*
 * These return LZN_FAIL without setting an error when the value is of another kind, and never
 * convert. Use the lzn_check_* functions to turn that into an error.
 */
LznStatus lzn_to_int(LznCtx *ctx, LznValue *value, int64_t *out);
LznStatus lzn_to_float(LznCtx *ctx, LznValue *value, double *out); /* an int is not a float here */
LznStatus lzn_to_bool(LznCtx *ctx, LznValue *value, bool *out);

/* Borrowed and NUL-terminated. Valid until the native function returns, and not to be written to. */
const char *lzn_string_data(LznCtx *ctx, LznValue *value, size_t *length);


size_t lzn_array_len(LznCtx *ctx, LznValue *array);
LznValue *lzn_array_get(LznCtx *ctx, LznValue *array, size_t index); /* NULL when out of range */
LznStatus lzn_array_push(LznCtx *ctx, LznValue *array, LznValue *value);
LznStatus lzn_array_set(LznCtx *ctx, LznValue *array, size_t index, LznValue *value);

size_t lzn_dict_len(LznCtx *ctx, LznValue *dict);
LznValue *lzn_dict_get(LznCtx *ctx, LznValue *dict, const char *key); /* NULL when missing */
LznValue *lzn_dict_get_int(LznCtx *ctx, LznValue *dict, int64_t key);
bool lzn_dict_has(LznCtx *ctx, LznValue *dict, const char *key);
bool lzn_dict_has_int(LznCtx *ctx, LznValue *dict, int64_t key);
LznStatus lzn_dict_set(LznCtx *ctx, LznValue *dict, const char *key, LznValue *value);
LznStatus lzn_dict_set_int(LznCtx *ctx, LznValue *dict, int64_t key, LznValue *value);


/*
 * A strong reference. The VM releases the ones still alive when it is freed, after the finalizers
 * ran. A reference that closes a cycle is not seen by the cycle collector, so the module lets go of
 * it on its own. Never call these from another thread.
 */
LznRef *lzn_ref(LznCtx *ctx, LznValue *value); /* NULL with an error set when out of memory */
LznValue *lzn_deref(LznCtx *ctx, LznRef *ref); /* a new handle in the current call */
void lzn_unref(LznRef *ref);                   /* NULL is fine */


/* Sets the error and returns LZN_FAIL, so `return lzn_throw(...)` is the usual form. */
LznStatus lzn_throw(LznCtx *ctx, LznErrId kind, const char *fmt, ...) LZN_PRINTF(3, 4);

/*
 * Each one reports the parameter name and expected type and returns LZN_FAIL when argument `index`
 * is of another kind, else LZN_OK and the value in *out. The _opt forms accept nil or a missing
 * argument and leave *out as it was.
 */
LznStatus lzn_check_int(LznCtx *ctx, int index, int64_t *out);
LznStatus lzn_check_float(LznCtx *ctx, int index, double *out); /* accepts an int too */
LznStatus lzn_check_bool(LznCtx *ctx, int index, bool *out);
LznStatus lzn_check_string(LznCtx *ctx, int index, const char **data, size_t *length);
LznStatus lzn_check_array(LznCtx *ctx, int index, LznValue **out);
LznStatus lzn_check_dict(LznCtx *ctx, int index, LznValue **out);
LznStatus lzn_check_function(LznCtx *ctx, int index, LznValue **out);


/*
 * Calls a Lizard function or closure. On LZN_FAIL the error is set and the native function returns
 * it to its caller as it is. A native that calls back cannot be suspended. Not allowed inside a
 * finalizer.
 */
LznStatus lzn_call(LznCtx *ctx, LznValue *fn, int nargs, LznValue *const *args, LznValue **result);


/* Counted against --max-heap. NULL with the usual error set when over the limit. */
void *lzn_alloc(LznCtx *ctx, size_t size);
void *lzn_realloc(LznCtx *ctx, void *block, size_t old_size, size_t new_size);
void lzn_free(LznCtx *ctx, void *block, size_t size);
/* Charges `units` of fuel. LZN_FAIL with the usual "instruction limit exceeded" when out. */
LznStatus lzn_spend(LznCtx *ctx, uint64_t units);


/*
 * The signature determines argument names, types, defaults and arity, for example
 * "fn(a: int, b: int = 0) -> int" or "fn(*rest: any)". Defaults must be literals.
 * The VM fills omitted defaults before calling native code. lzn_arg_count includes
 * every fixed parameter and each supplied rest argument. Methods omit self.
 * Unnamed parameter types remain accepted with names arg1, arg2 and so on.
 * Invalid signatures fail module registration.
 */
LznStatus lzn_module_fn(LznBuilder *b, const char *name, LznFn fn, const char *signature);
LznStatus lzn_module_const_int(LznBuilder *b, const char *name, int64_t value);
LznStatus lzn_module_const_float(LznBuilder *b, const char *name, double value);
LznStatus lzn_module_const_bool(LznBuilder *b, const char *name, bool value);
LznStatus lzn_module_const_string(LznBuilder *b, const char *name, const char *value);

/* Declares `error Name : Parent` in the module. parent is a built-in kind or an earlier module kind. */
LznErrId lzn_module_error(LznBuilder *b, const char *name, LznErrId parent); /* -1 on failure */

typedef struct LznMethodDef {
    const char *name;
    LznFn fn;
    const char *signature; /* as for lzn_module_fn, without self */
} LznMethodDef;

typedef struct LznFieldDef {
    const char *name;
    const char *type;    /* Lizard type text like "int" or "string?" */
    LznFn get;           /* returns the value of the field */
} LznFieldDef;

typedef struct LznClassDef {
    size_t size;                          /* bytes of opaque data in every instance */
    LznFn init;                           /* the constructor, may be NULL */
    const char *init_signature;           /* for example "fn(start: int = 0)", without a result */
    void (*finalize)(void *data);         /* once per instance, may be NULL */
    const LznMethodDef *methods;          /* ended by an entry with name NULL, or NULL */
    const LznFieldDef *fields;            /* read-only fields, ended by name NULL, or NULL */
} LznClassDef;

/* The class name can then be used in signatures of this module. */
LznStatus lzn_module_class(LznBuilder *b, const char *name, const LznClassDef *def);

/* The module's own name, for the init to use in messages. Valid while init runs. */
const char *lzn_module_name(LznBuilder *b);

/* Inside init, init-time failures are reported with this. */
LznStatus lzn_builder_fail(LznBuilder *b, const char *fmt, ...) LZN_PRINTF(2, 3);

/* The data of a native class's instance. Only inside its init, methods and field getters. */
void *lzn_self_data(LznCtx *ctx);

#ifdef __cplusplus
}
#endif

#endif
