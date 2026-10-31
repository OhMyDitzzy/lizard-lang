#ifndef LIZARD_BUILTIN_METHOD_H
#define LIZARD_BUILTIN_METHOD_H

#include "lizard/native.h"
#include "lizard/vm.h"

/*
 * Methods bound to the kind of a value: `"Ab".to_lower()`, `2.5.round()`.
 *
 * Each implementation is a plain native whose argv[0] is the receiver. All the
 * implementations of one name (to_string for every kind, is_empty for strings and
 * arrays, ...) share one dispatcher native in vm->methods; it picks the row whose kinds
 * include the receiver's. A kind without a row falls back to the global function of the
 * same name, so `xs.contains(3)` still reaches the builtin `contains(xs, v)` while
 * `"abc".contains("b")` finds the string method.
 */

typedef struct LzMethod {
    const char *name;
    uint16_t kinds;      /* the kinds of receiver this row serves */
    LzNativeFn fn;       /* argv[0] is the receiver */
    int min_args;        /* arguments after the receiver */
    int max_args;
    uint16_t result_kinds;
} LzMethod;

/* Creates the dispatchers and defines them in vm->methods. Called by lz_builtin_install. */
LzStatus lz_method_install(LzVM *vm);

/* Implementations (text.c, number.c, method.c). */
LzStatus lz_method_to_string(LzVM *vm, int argc, const LzValue *argv, LzValue *result);
LzStatus lz_method_format(LzVM *vm, int argc, const LzValue *argv, LzValue *result);
LzStatus lz_method_to_debug(LzVM *vm, int argc, const LzValue *argv, LzValue *result);
LzStatus lz_method_to_upper(LzVM *vm, int argc, const LzValue *argv, LzValue *result);
LzStatus lz_method_to_lower(LzVM *vm, int argc, const LzValue *argv, LzValue *result);
LzStatus lz_method_trim(LzVM *vm, int argc, const LzValue *argv, LzValue *result);
LzStatus lz_method_trim_start(LzVM *vm, int argc, const LzValue *argv, LzValue *result);
LzStatus lz_method_trim_end(LzVM *vm, int argc, const LzValue *argv, LzValue *result);
LzStatus lz_method_split(LzVM *vm, int argc, const LzValue *argv, LzValue *result);
LzStatus lz_method_contains(LzVM *vm, int argc, const LzValue *argv, LzValue *result);
LzStatus lz_method_starts_with(LzVM *vm, int argc, const LzValue *argv, LzValue *result);
LzStatus lz_method_ends_with(LzVM *vm, int argc, const LzValue *argv, LzValue *result);
LzStatus lz_method_replace(LzVM *vm, int argc, const LzValue *argv, LzValue *result);
LzStatus lz_method_repeat(LzVM *vm, int argc, const LzValue *argv, LzValue *result);
LzStatus lz_method_index_of(LzVM *vm, int argc, const LzValue *argv, LzValue *result);
LzStatus lz_method_chars(LzVM *vm, int argc, const LzValue *argv, LzValue *result);
LzStatus lz_method_slice(LzVM *vm, int argc, const LzValue *argv, LzValue *result);
LzStatus lz_method_is_empty(LzVM *vm, int argc, const LzValue *argv, LzValue *result);
LzStatus lz_method_first(LzVM *vm, int argc, const LzValue *argv, LzValue *result);
LzStatus lz_method_last(LzVM *vm, int argc, const LzValue *argv, LzValue *result);
LzStatus lz_method_abs(LzVM *vm, int argc, const LzValue *argv, LzValue *result);
LzStatus lz_method_floor(LzVM *vm, int argc, const LzValue *argv, LzValue *result);
LzStatus lz_method_ceil(LzVM *vm, int argc, const LzValue *argv, LzValue *result);
LzStatus lz_method_round(LzVM *vm, int argc, const LzValue *argv, LzValue *result);
LzStatus lz_method_sqrt(LzVM *vm, int argc, const LzValue *argv, LzValue *result);
LzStatus lz_method_pow(LzVM *vm, int argc, const LzValue *argv, LzValue *result);
LzStatus lz_method_min(LzVM *vm, int argc, const LzValue *argv, LzValue *result);
LzStatus lz_method_max(LzVM *vm, int argc, const LzValue *argv, LzValue *result);

#endif
