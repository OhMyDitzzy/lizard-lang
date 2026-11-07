#ifndef LIZARD_BUILTIN_CONVERT_H
#define LIZARD_BUILTIN_CONVERT_H

#include "lizard/builtin.h"

LzStatus lz_builtin_int(LzVM *vm, int argc, const LzValue *argv, LzValue *result);
LzStatus lz_builtin_float(LzVM *vm, int argc, const LzValue *argv, LzValue *result);
LzStatus lz_builtin_string(LzVM *vm, int argc, const LzValue *argv, LzValue *result);
LzStatus lz_builtin_bool(LzVM *vm, int argc, const LzValue *argv, LzValue *result);
LzStatus lz_builtin_identical(LzVM *vm, int argc, const LzValue *argv, LzValue *result);
LzStatus lz_builtin_link(LzVM *vm, int argc, const LzValue *argv, LzValue *result);
LzStatus lz_builtin_target(LzVM *vm, int argc, const LzValue *argv, LzValue *result);
LzStatus lz_builtin_alive(LzVM *vm, int argc, const LzValue *argv, LzValue *result);

#endif
