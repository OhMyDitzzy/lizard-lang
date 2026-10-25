#include "lizard/vm.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

void lz_vm_init(LzVM *vm, LzWriter out, LzWriter err) {
    lz_heap_init(&vm->heap);
    lz_table_init(&vm->strings);
    lz_table_init(&vm->globals);
    lz_table_init(&vm->builtins);
    vm->array_types = NULL;
    vm->array_type_count = 0;
    vm->array_type_capacity = 0;
    vm->stack_count = 0;
    vm->frame_count = 0;
    vm->open_upvalues = NULL;
    vm->fuel = LZ_FUEL_UNLIMITED;
    vm->instructions = 0;
    vm->error_offset = LZ_NO_OFFSET;
    vm->trace_count = 0;
    vm->trace_total = 0;
    vm->out = out;
    vm->err = err;
    vm->error[0] = '\0';
    lz_skink_init(&vm->skink);
    vm->heap.reclaim = lz_skink_reclaim; /* before --max-heap refuses an allocation */
    vm->heap.reclaim_context = vm;
}

void lz_vm_set_heap_limit(LzVM *vm, size_t bytes) {
    vm->heap.limit_bytes = bytes;
}

void lz_vm_set_fuel(LzVM *vm, uint64_t fuel) {
    vm->fuel = fuel;
}

LzStatus lz_vm_out_of_memory(LzVM *vm) {
    if (vm->heap.limit_hit) {
        vm->heap.limit_hit = false;
        return lz_vm_set_error(vm, LZ_ERR_HEAP_LIMIT, "heap limit of %zu bytes exceeded",
                               vm->heap.limit_bytes);
    }
    return lz_vm_set_error(vm, LZ_ERR_MEMORY, "out of memory");
}

void lz_vm_reset_stack(LzVM *vm) {
    lz_vm_close_upvalues(vm, 0); /* before the slots die: a closure may outlive them */
    vm->frame_count = 0;
    while (vm->stack_count > 0) {
        vm->stack_count--;
        lz_value_release(&vm->heap, &vm->stack[vm->stack_count]);
    }
    lz_skink_flush(vm); /* the suspects closing reported hold references of their own */
}

void lz_vm_release_roots(LzVM *vm) {
    lz_vm_reset_stack(vm);
    lz_table_free(&vm->heap, &vm->builtins);
    lz_table_free(&vm->heap, &vm->globals);
    lz_table_free(&vm->heap, &vm->strings);
    lz_skink_sweep(vm); /* cycles that were only waiting for their last holder; arrays still read their types */
    for (size_t i = 0; i < vm->array_type_count; i++) {
        free(vm->array_types[i]);
    }
    free(vm->array_types);
    vm->array_types = NULL;
    vm->array_type_count = 0;
    vm->array_type_capacity = 0;
}

void lz_vm_free(LzVM *vm) {
    lz_writer_flush(&vm->out);
    lz_writer_flush(&vm->err);
    lz_vm_release_roots(vm);
    lz_heap_destroy(&vm->heap);
}

LzString *lz_vm_intern(LzVM *vm, const char *chars, size_t length) {
    uint32_t hash = lz_hash_bytes(chars, length);
    LzString *existing = lz_table_find_string(&vm->strings, chars, length, hash);
    if (existing != NULL) {
        return existing;
    }

    LzString *string = lz_string_new(&vm->heap, chars, length);
    if (string == NULL) {
        return NULL;
    }
    bool stored = lz_table_set(&vm->heap, &vm->strings, string, lz_value_nil());
    lz_object_release(&vm->heap, &string->base);
    return stored ? string : NULL;
}

bool lz_vm_define_global(LzVM *vm, LzString *name, LzValue value) {
    return lz_table_set(&vm->heap, &vm->globals, name, value);
}

bool lz_vm_define_builtin(LzVM *vm, LzString *name, LzValue value) {
    return lz_table_set(&vm->heap, &vm->builtins, name, value);
}

const LzValue *lz_vm_lookup(const LzVM *vm, const LzString *name) {
    const LzValue *value = lz_table_get(&vm->globals, name);
    return value != NULL ? value : lz_table_get(&vm->builtins, name);
}

LzStatus lz_vm_set_error(LzVM *vm, LzStatus status, const char *format, ...) {
    va_list args;
    va_start(args, format);
    (void)vsnprintf(vm->error, sizeof vm->error, format, args);
    va_end(args);
    return status;
}
