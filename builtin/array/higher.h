#ifndef LIZARD_BUILTIN_HIGHER_H
#define LIZARD_BUILTIN_HIGHER_H

#include "lizard/native.h"
#include "lizard/vm.h"

/*
 * Higher-order array functions: they call a function value for the items of an array
 * (see lz_vm_call_value). Each is also what `xs.name(f)` means.
 */
LzStatus lz_builtin_map(LzVM *vm, int argc, const LzValue *argv, LzValue *result);
LzStatus lz_builtin_filter(LzVM *vm, int argc, const LzValue *argv, LzValue *result);
LzStatus lz_builtin_reduce(LzVM *vm, int argc, const LzValue *argv, LzValue *result);
LzStatus lz_builtin_each(LzVM *vm, int argc, const LzValue *argv, LzValue *result);
LzStatus lz_builtin_find(LzVM *vm, int argc, const LzValue *argv, LzValue *result);
LzStatus lz_builtin_any(LzVM *vm, int argc, const LzValue *argv, LzValue *result);
LzStatus lz_builtin_all(LzVM *vm, int argc, const LzValue *argv, LzValue *result);

#endif
