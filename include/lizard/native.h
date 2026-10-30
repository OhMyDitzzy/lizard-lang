#ifndef LIZARD_NATIVE_H
#define LIZARD_NATIVE_H

#include "lizard/type.h"
#include "lizard/value.h"

#define LZ_VARIADIC (-1)

/*
 * A function implemented in C. It receives already-evaluated runtime values
 * and reports failure through the returned status plus lz_vm_set_error. It has
 * no knowledge of tokens, syntax or the parser.
 *
 * On success *result is a new reference: the caller owns it. A function that
 * returns one of its arguments must retain it first.
 */
typedef LzStatus (*LzNativeFn)(LzVM *vm, int argc, const LzValue *argv, LzValue *result);

typedef struct LzNative {
    LzObject base;
    LzString *name;
    LzNativeFn fn;
    int min_args;
    int max_args;
    uint16_t result_kinds; /* what a call can return; LZ_KINDS_ANY when unknown */
    const void *data;      /* private to whoever made the native (a method dispatcher keeps its group here) */
} LzNative;

/* The returned object holds its own reference to name. */
LzNative *lz_native_new(LzHeap *heap, LzString *name, LzNativeFn fn, int min_args, int max_args);

/* Validates arity, then invokes the function. */
LzStatus lz_native_call(LzVM *vm, const LzNative *native, int argc, const LzValue *argv,
                        LzValue *result);

#endif
