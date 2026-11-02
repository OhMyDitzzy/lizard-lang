#ifndef LIZARD_BUILTIN_ERROR_H
#define LIZARD_BUILTIN_ERROR_H

#include "lizard/builtin.h"

/* Error("message"), RuntimeError("message"), ...: one function per kind. */
LzStatus lz_builtin_Error(LzVM *vm, int argc, const LzValue *argv, LzValue *result);
LzStatus lz_builtin_RuntimeError(LzVM *vm, int argc, const LzValue *argv, LzValue *result);
LzStatus lz_builtin_TypeError(LzVM *vm, int argc, const LzValue *argv, LzValue *result);
LzStatus lz_builtin_ValueError(LzVM *vm, int argc, const LzValue *argv, LzValue *result);
LzStatus lz_builtin_RangeError(LzVM *vm, int argc, const LzValue *argv, LzValue *result);
LzStatus lz_builtin_ArithmeticError(LzVM *vm, int argc, const LzValue *argv, LzValue *result);
LzStatus lz_builtin_ArityError(LzVM *vm, int argc, const LzValue *argv, LzValue *result);
LzStatus lz_builtin_IOError(LzVM *vm, int argc, const LzValue *argv, LzValue *result);

#endif
