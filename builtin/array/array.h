#ifndef LIZARD_BUILTIN_ARRAY_H
#define LIZARD_BUILTIN_ARRAY_H

#include "lizard/native.h"
#include "lizard/vm.h"

LzStatus lz_builtin_len(LzVM *vm, int argc, const LzValue *argv, LzValue *result);
LzStatus lz_builtin_push(LzVM *vm, int argc, const LzValue *argv, LzValue *result);
LzStatus lz_builtin_pop(LzVM *vm, int argc, const LzValue *argv, LzValue *result);
LzStatus lz_builtin_insert(LzVM *vm, int argc, const LzValue *argv, LzValue *result);
LzStatus lz_builtin_remove(LzVM *vm, int argc, const LzValue *argv, LzValue *result);
LzStatus lz_builtin_clear(LzVM *vm, int argc, const LzValue *argv, LzValue *result);
LzStatus lz_builtin_copy(LzVM *vm, int argc, const LzValue *argv, LzValue *result);
LzStatus lz_builtin_contains(LzVM *vm, int argc, const LzValue *argv, LzValue *result);
LzStatus lz_builtin_index_of(LzVM *vm, int argc, const LzValue *argv, LzValue *result);
LzStatus lz_builtin_reverse(LzVM *vm, int argc, const LzValue *argv, LzValue *result);
LzStatus lz_builtin_sort(LzVM *vm, int argc, const LzValue *argv, LzValue *result);
LzStatus lz_builtin_join(LzVM *vm, int argc, const LzValue *argv, LzValue *result);
LzStatus lz_builtin_range(LzVM *vm, int argc, const LzValue *argv, LzValue *result);
LzStatus lz_builtin_sum(LzVM *vm, int argc, const LzValue *argv, LzValue *result);
LzStatus lz_builtin_min(LzVM *vm, int argc, const LzValue *argv, LzValue *result);
LzStatus lz_builtin_max(LzVM *vm, int argc, const LzValue *argv, LzValue *result);

#endif
