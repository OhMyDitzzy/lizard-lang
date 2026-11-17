#ifndef LIZARD_COROUTINE_H
#define LIZARD_COROUTINE_H

#include "lizard/vm.h"

typedef enum LzCoroutineState {
    LZ_CO_SUSPENDED,
    LZ_CO_RUNNING,
    LZ_CO_NORMAL,
    LZ_CO_DONE,
    LZ_CO_FAILED
} LzCoroutineState;

typedef struct LzCoroutine {
    LzObject base;
    LzVM *vm;
    LzExecutionContext context;
    LzValue entry;
    LzCoroutineState state;
    unsigned yield_depth;
} LzCoroutine;

const char *lz_coroutine_status(const LzCoroutine *co);
LzStatus lz_builtin_coroutine(LzVM *vm, int argc, const LzValue *argv, LzValue *result);
LzStatus lz_builtin_yield(LzVM *vm, int argc, const LzValue *argv, LzValue *result);
LzStatus lz_coroutine_resume(LzVM *vm, int argc, const LzValue *argv, LzValue *result);
LzStatus lz_coroutine_get_status(LzVM *vm, int argc, const LzValue *argv, LzValue *result);
void lz_coroutine_clear(LzCoroutine *co);

#endif
