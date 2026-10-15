#ifndef LIZARD_BUILTIN_IO_H
#define LIZARD_BUILTIN_IO_H

#include "lizard/builtin.h"

LzStatus lz_builtin_print(LzVM *vm, int argc, const LzValue *argv, LzValue *result);
LzStatus lz_builtin_println(LzVM *vm, int argc, const LzValue *argv, LzValue *result);

#endif
