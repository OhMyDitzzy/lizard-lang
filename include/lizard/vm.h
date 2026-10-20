#ifndef LIZARD_VM_H
#define LIZARD_VM_H

#include "lizard/chunk.h"
#include "lizard/common.h"
#include "lizard/memory.h"
#include "lizard/table.h"
#include "lizard/writer.h"

#define LZ_STACK_MAX 1024

struct LzVM {
    LzHeap heap;
    LzWriter out;
    LzWriter err;
    LzTable strings;
    LzTable globals;
    LzTable builtins;
    LzType **array_types; /* interned item types of array types, see lz_vm_array_of */
    size_t array_type_count;
    size_t array_type_capacity;
    LzValue stack[LZ_STACK_MAX];
    size_t stack_count;
    uint64_t fuel;
    uint64_t instructions;
    uint32_t error_offset; /* source offset of the failing instruction, or LZ_NO_OFFSET */
    char error[256];
};

#define LZ_FUEL_UNLIMITED UINT64_MAX

void lz_vm_init(LzVM *vm, LzWriter out, LzWriter err);

/* Caps live heap bytes for this VM; LZ_HEAP_UNLIMITED (the default) removes the cap. */
void lz_vm_set_heap_limit(LzVM *vm, size_t bytes);

/*
 * Sets the remaining instruction budget. Each executed instruction spends one
 * unit, and the budget persists across lz_vm_run calls, so hosts set it before
 * every run. LZ_FUEL_UNLIMITED (the default) turns metering off.
 * vm->instructions counts executed instructions either way.
 */
void lz_vm_set_fuel(LzVM *vm, uint64_t fuel);

/*
 * Executes a chunk from its first byte until OP_RETURN. On failure the status
 * is returned, vm->error holds the message and vm->error_offset the source
 * offset of the failing instruction. The value stack is always empty afterwards.
 */
LzStatus lz_vm_run(LzVM *vm, const LzChunk *chunk);

/*
 * Like lz_vm_run, but on success stores the value left on top of the stack
 * (nil if the stack is empty) in *result as a new reference the caller owns.
 */
LzStatus lz_vm_eval(LzVM *vm, const LzChunk *chunk, LzValue *result);

/* Pops and releases every value on the stack. */
void lz_vm_reset_stack(LzVM *vm);

/*
 * Drops every reference the VM itself holds (interned names, globals,
 * builtins). Afterwards heap.live_objects counts objects that leaked.
 */
void lz_vm_release_roots(LzVM *vm);

/* Flushes both writers, releases the roots and frees the heap. */
void lz_vm_free(LzVM *vm);

/*
 * Returns the unique string for these characters. The intern table owns the
 * reference, so the caller must not release the result. NULL when out of memory.
 */
LzString *lz_vm_intern(LzVM *vm, const char *chars, size_t length);

bool lz_vm_define_global(LzVM *vm, LzString *name, LzValue value);
bool lz_vm_define_builtin(LzVM *vm, LzString *name, LzValue value);

/* Globals shadow builtins. name must be interned. NULL when undefined. */
const LzValue *lz_vm_lookup(const LzVM *vm, const LzString *name);

/*
 * Status for a failed allocation: LZ_ERR_HEAP_LIMIT when the configured limit
 * refused it, LZ_ERR_MEMORY otherwise. Natives call this when lz_heap_alloc
 * or lz_string_new returns NULL.
 */
LzStatus lz_vm_out_of_memory(LzVM *vm);

/* Records a message in vm->error and returns status so callers can `return` it directly. */
LzStatus lz_vm_set_error(LzVM *vm, LzStatus status, const char *format, ...) LZ_PRINTF_FORMAT(3, 4);

/*
 * Spends cost units of fuel; natives that do heavy work call this so one
 * instruction cannot hide unbounded effort. Fails with LZ_ERR_FUEL and leaves
 * the budget at zero when it cannot cover the cost.
 */
static inline LzStatus lz_vm_charge_fuel(LzVM *vm, uint64_t cost) {
    if (vm->fuel == LZ_FUEL_UNLIMITED) {
        return LZ_OK;
    }
    if (cost > vm->fuel) {
        vm->fuel = 0;
        return lz_vm_set_error(vm, LZ_ERR_FUEL, "instruction limit exceeded");
    }
    vm->fuel -= cost;
    return LZ_OK;
}

#endif
