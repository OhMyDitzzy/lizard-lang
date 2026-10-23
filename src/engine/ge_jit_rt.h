#ifndef LIZARD_GE_JIT_RT_H
#define LIZARD_GE_JIT_RT_H

/* Runtime support called from generated code (every backend), and executable memory. */

#include "ge_jit.h"

void jit_drop(LzVM *vm, LzObject *object);
void jit_drop_range(LzVM *vm, LzValue *first, uint64_t count);
const LzValue *jit_global_cell(LzVM *vm, const LzString *name, JitSite *site);
const LzValue *jit_global_ptr(LzVM *vm, const LzString *name, JitSite *site);
uint64_t jit_call(LzVM *vm, uint64_t argc, uint64_t resume, uint64_t call_offset, void **entry);
uint32_t jit_delegate(LzVM *vm, const LzChunk *chunk, uint64_t byte_offset);

/* Copies code into fresh pages and makes them executable (never writable at the same time). */
void *jit_exec_alloc(const uint8_t *code, size_t size, size_t *mapped);

#endif
