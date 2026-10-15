#ifndef LIZARD_BUILTIN_H
#define LIZARD_BUILTIN_H

#include "lizard/native.h"
#include "lizard/vm.h"

/* Declarative description of one builtin; install turns each into an LzNative. */
typedef struct LzBuiltin {
    const char *name;
    LzNativeFn fn;
    int min_args;
    int max_args;
} LzBuiltin;

const LzBuiltin *lz_builtin_lookup(const char *name);
size_t lz_builtin_count(void);
const LzBuiltin *lz_builtin_at(size_t index);

/* Creates a native object for every builtin and defines it in the VM's builtin scope. */
LzStatus lz_builtin_install(LzVM *vm);

#endif
