#include "lizard/vm.h"
#include "lizard/class.h"
#include "lizard/link.h"
#include "lizard/module.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

void lz_vm_init(LzVM *vm, LzWriter out, LzWriter err) {
    lz_heap_init(&vm->heap);
    lz_table_init(&vm->strings);
    lz_table_init(&vm->globals);
    lz_modules_init(vm);
    vm->main_module = calloc(1, sizeof *vm->main_module);
    if (vm->main_module != NULL) {
        lz_table_init(&vm->main_module->globals); /* unused, main's globals are vm->globals */
        vm->main_module->state = LZ_MODULE_READY;
        vm->main_module->is_main = true;
        vm->modules = vm->main_module;
        vm->module_count = 1;
    }
    lz_table_init(&vm->builtins);
    lz_table_init(&vm->methods);
    vm->current_native = NULL;
    vm->array_types = NULL;
    vm->array_type_count = 0;
    vm->array_type_capacity = 0;
    vm->fn_types = NULL;
    vm->fn_type_count = 0;
    vm->fn_type_capacity = 0;
    vm->stack_count = 0;
    vm->frame_count = 0;
    vm->open_upvalues = NULL;
    vm->fuel = LZ_FUEL_UNLIMITED;
    vm->instructions = 0;
    vm->error_offset = LZ_NO_OFFSET;
    vm->error_source = NULL;
    vm->native_call_offset = LZ_NO_OFFSET;
    vm->native_depth = 0;
    vm->c_stack_floor = 0;
    vm->c_stack_lo = 0;
    vm->c_stack_hi = 0;
    vm->trace_fixed = false;
    vm->trace_count = 0;
    vm->trace_total = 0;
    vm->out = out;
    vm->err = err;
    vm->error[0] = '\0';
    vm->error_kind = LZ_ERROR_RUNTIME;
    vm->thrown = lz_value_nil();
    vm->last_thrown = lz_value_nil();
    vm->error_type_count = 0;
    vm->handler_count = 0;
    vm->trace_source = NULL;
    vm->error_suggestions.count = 0;
    lz_vm_clear_error_extras(vm);
    vm->text_status = LZ_OK;
    vm->has_classes = false;
    vm->exts = NULL;
    vm->ext_count = 0;
    vm->ext_capacity = 0;
    vm->ext_version = 0;
    vm->method_row_check = NULL;
    vm->next_class_id = 0;
    vm->class_registry = NULL;
    vm->class_registry_count = 0;
    vm->class_registry_capacity = 0;
    vm->native_with = NULL;
    vm->native_copy = NULL;
    vm->link_buckets = NULL;
    vm->link_bucket_count = 0;
    vm->link_count = 0;
    vm->link_targets = 0;
    vm->link_cancelled = 0;
    lz_skink_init(&vm->skink);
    vm->heap.reclaim = lz_skink_reclaim; /* before --max-heap refuses an allocation */
    vm->heap.reclaim_context = vm;
    vm->heap.on_alloc = lz_skink_alloc_tick; /* a tick every LZ_SKINK_ALLOC_PERIOD allocations */
    vm->heap.on_alloc_context = vm;
    vm->heap.alloc_period = LZ_SKINK_ALLOC_PERIOD;
    vm->heap.alloc_countdown = LZ_SKINK_ALLOC_PERIOD;
    vm->heap.on_link_freed = lz_link_release_hook; /* a dying link target empties the links to it */
    vm->heap.on_link_freed_context = vm;
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
    vm->handler_count = 0;
    if (vm->thrown.type != LZ_VAL_NIL) {
        /* The run failed with this error, so keep it because a report wants its causes. */
        LzValue old = vm->last_thrown;
        vm->last_thrown = vm->thrown;
        vm->thrown = lz_value_nil();
        lz_value_release(&vm->heap, &old);
    }
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
    if (vm->last_thrown.type != LZ_VAL_NIL) {
        LzValue last = vm->last_thrown;
        vm->last_thrown = lz_value_nil();
        lz_value_release(&vm->heap, &last);
    }
    lz_vm_release_error_classes(vm); /* before the count goes: the classes are retained per kind */
    vm->error_type_count = 0; /* the names go with the intern table below */
    lz_table_free(&vm->heap, &vm->methods);
    lz_table_free(&vm->heap, &vm->builtins);
    lz_table_free(&vm->heap, &vm->globals);
    lz_modules_release_tables(vm); /* module globals are roots too, and they go before the heap does */
    lz_table_free(&vm->heap, &vm->strings);
    lz_vm_release_extensions(vm);
    lz_vm_release_class_constants(vm); /* a constant may be an instance of its own class */
    if (vm->native_with != NULL) {
        lz_object_release(&vm->heap, vm->native_with);
        vm->native_with = NULL;
    }
    if (vm->native_copy != NULL) {
        lz_object_release(&vm->heap, vm->native_copy);
        vm->native_copy = NULL;
    }
    lz_skink_sweep(vm); /* cycles that were only waiting for their last holder, and arrays still read their types */
    for (size_t i = 0; i < vm->array_type_count; i++) {
        free(vm->array_types[i]);
    }
    free(vm->array_types);
    vm->array_types = NULL;
    vm->array_type_count = 0;
    vm->array_type_capacity = 0;
    for (size_t i = 0; i < vm->fn_type_count; i++) {
        free(vm->fn_types[i]);
    }
    free(vm->fn_types);
    vm->fn_types = NULL;
    vm->fn_type_count = 0;
    vm->fn_type_capacity = 0;
}

void lz_vm_free(LzVM *vm) {
    lz_writer_flush(&vm->out);
    lz_writer_flush(&vm->err);
    lz_vm_release_roots(vm);
    lz_heap_destroy(&vm->heap);
    lz_link_table_free(vm);
    free(vm->class_registry);
    vm->class_registry = NULL;
    lz_modules_free(vm); /* last: chunks and classes keep borrowed pointers to modules until the heap is gone */
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

bool lz_vm_define_method(LzVM *vm, LzString *name, LzValue value) {
    return lz_table_set(&vm->heap, &vm->methods, name, value);
}

const LzValue *lz_vm_lookup(const LzVM *vm, const LzString *name) {
    return lz_module_lookup(vm, &vm->globals, name);
}

bool lz_vm_take_text_status(LzVM *vm, LzStatus *status) {
    if (vm->text_status == LZ_OK) {
        return false;
    }
    *status = vm->text_status;
    vm->text_status = LZ_OK;
    return true;
}

LzStatus lz_vm_set_error(LzVM *vm, LzStatus status, const char *format, ...) {
    va_list args;
    vm->error_suggestions.count = 0;
    lz_vm_clear_error_extras(vm);
    va_start(args, format);
    (void)vsnprintf(vm->error, sizeof vm->error, format, args);
    va_end(args);
    vm->error_kind = (uint8_t)lz_error_classify(status, vm->error);
    if (vm->thrown.type != LZ_VAL_NIL) {
        /* A new failure replaces the error value that was being carried. */
        LzValue old = vm->thrown;
        vm->thrown = lz_value_nil();
        lz_value_release(&vm->heap, &old);
    }
    return status;
}

void lz_vm_set_help(LzVM *vm, const char *format, ...) {
    va_list args;
    va_start(args, format);
    (void)vsnprintf(vm->error_help, sizeof vm->error_help, format, args);
    va_end(args);
}

void lz_vm_set_note(LzVM *vm, const char *format, ...) {
    va_list args;
    va_start(args, format);
    (void)vsnprintf(vm->error_note, sizeof vm->error_note, format, args);
    va_end(args);
}

void lz_vm_set_label(LzVM *vm, const char *format, ...) {
    va_list args;
    va_start(args, format);
    (void)vsnprintf(vm->error_label, sizeof vm->error_label, format, args);
    va_end(args);
}

void lz_vm_set_fix(LzVM *vm, const char *headline, uint32_t offset, size_t delete_length,
                   const char *insert_text) {
    LzVMFix *fix = &vm->error_fix;
    fix->present = true;
    fix->offset = offset;
    fix->delete_length = (uint32_t)delete_length;
    (void)snprintf(fix->headline, sizeof fix->headline, "%s", headline);
    (void)snprintf(fix->insert_text, sizeof fix->insert_text, "%s", insert_text);
}

void lz_vm_clear_error_extras(LzVM *vm) {
    vm->error_help[0] = '\0';
    vm->error_note[0] = '\0';
    vm->error_label[0] = '\0';
    vm->error_fix.present = false;
}
