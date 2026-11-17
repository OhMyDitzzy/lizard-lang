#include "lizard/coroutine.h"
#include "lizard/array.h"

#include <stdio.h>
#include <string.h>

const char *lz_coroutine_status(const LzCoroutine *co) {
    static const char *const names[] = {"suspended", "running", "normal", "done", "failed"};
    return names[co->state];
}

static LzStatus pack(LzVM *vm, size_t count, const LzValue *values, LzValue *out) {
    *out = lz_value_nil();
    if (count == 1) {
        *out = values[0];
        lz_value_retain(out);
    } else if (count > 1) {
        LzArray *array = lz_array_new(&vm->heap, count);
        if (array == NULL) {
            return lz_vm_out_of_memory(vm);
        }
        for (size_t i = 0; i < count; i++) {
            LzStatus status = lz_array_push(vm, array, &values[i]);
            if (status != LZ_OK) {
                lz_object_release(&vm->heap, &array->base);
                return status;
            }
        }
        *out = lz_value_object(&array->base);
    }
    return LZ_OK;
}

LzStatus lz_builtin_coroutine(LzVM *vm, int argc, const LzValue *argv, LzValue *result) {
    if (!lz_value_is_closure(&argv[0])) {
        return lz_vm_set_error(vm, LZ_ERR_TYPE, "coroutine(): expected a Lizard function, got %s",
                               lz_value_kind_name(&argv[0]));
    }
    size_t depth = LZ_CO_DEPTH_DEFAULT;
    if (argc > 1) {
        LzStatus status = LZ_OK;
        if (argv[1].type != LZ_VAL_INT) {
            status = lz_vm_set_error(vm, LZ_ERR_TYPE, "coroutine(): depth must be an int, got %s",
                                     lz_value_kind_name(&argv[1]));
        } else if (argv[1].as.integer < 1 || argv[1].as.integer > LZ_FRAMES_MAX) {
            status = lz_vm_set_error(vm, LZ_ERR_RUNTIME, "coroutine(): depth must be between 1 and %d, got %lld",
                                     LZ_FRAMES_MAX, (long long)argv[1].as.integer);
            vm->error_kind = LZ_ERROR_VALUE;
        }
        if (status != LZ_OK) {
            lz_vm_set_help(vm, "use coroutine(fn, depth) with an int depth from 1 to %d (default %d)",
                           LZ_FRAMES_MAX, LZ_CO_DEPTH_DEFAULT);
            return status;
        }
        depth = (size_t)argv[1].as.integer;
    }
    LzCoroutine *co = lz_heap_alloc(&vm->heap, LZ_OBJ_COROUTINE, sizeof *co);
    if (co == NULL) {
        return lz_vm_out_of_memory(vm);
    }
    co->vm = vm;
    vm->skink.coroutine_count++;
    lz_vm_context_init(&co->context);
    lz_vm_context_set_depth(&co->context, depth);
    co->context.coroutine = co;
    co->entry = argv[0];
    lz_value_retain(&co->entry);
    co->state = LZ_CO_SUSPENDED;
    co->yield_depth = 0;
    *result = lz_value_object(&co->base);
    return LZ_OK;
}

void lz_coroutine_clear(LzCoroutine *co) {
    LzValue entry = co->entry;
    co->entry = lz_value_nil();
    lz_vm_context_free(co->vm, &co->context);
    lz_value_release(&co->vm->heap, &entry);
}

LzStatus lz_builtin_yield(LzVM *vm, int argc, const LzValue *argv, LzValue *result) {
    LzCoroutine *co = vm->context->coroutine;
    if (co == NULL) {
        return lz_vm_set_error(vm, LZ_ERR_RUNTIME, "cannot yield from the main program");
    }
    if (vm->native_depth != co->yield_depth) {
        return lz_vm_set_error(vm, LZ_ERR_RUNTIME, "cannot yield across a native call");
    }
    LzStatus status = pack(vm, (size_t)argc, argv, result);
    return status == LZ_OK ? LZ_YIELD : status;
}

LzStatus lz_coroutine_get_status(LzVM *vm, int argc, const LzValue *argv, LzValue *result) {
    (void)argc;
    const char *name = lz_coroutine_status((const LzCoroutine *)argv[0].as.object);
    LzString *text = lz_string_new(&vm->heap, name, strlen(name));
    if (text == NULL) {
        return lz_vm_out_of_memory(vm);
    }
    *result = lz_value_object(&text->base);
    return LZ_OK;
}

static void append_caller_trace(LzVM *vm, uint32_t call_site) {
    size_t floor = vm->context->coroutine == NULL ? 0 : 1;
    vm->trace_total += vm->frame_count - floor;
    uint32_t offset = LZ_NO_OFFSET;
    if (vm->frame_count > floor) {
        const LzChunk *chunk = vm->frames[vm->frame_count - 1].chunk;
        if (call_site < chunk->count) {
            offset = chunk->offsets[call_site];
        }
    }
    for (size_t i = vm->frame_count; i > floor && vm->trace_count < LZ_TRACE_MAX; i--) {
        const LzCallFrame *frame = &vm->frames[i - 1];
        LzTraceEntry *entry = &vm->trace[vm->trace_count++];
        (void)snprintf(entry->name, sizeof entry->name, "%s", frame->closure == NULL ? "<script>" :
                       lz_function_display_name(frame->closure->function));
        entry->offset = offset;
        entry->source = lz_chunk_source(frame->chunk);
        offset = LZ_NO_OFFSET;
        if (i > floor + 1 && frame->call_offset < vm->frames[i - 2].chunk->count) {
            offset = vm->frames[i - 2].chunk->offsets[frame->call_offset];
        }
    }
}

LzStatus lz_coroutine_resume(LzVM *vm, int argc, const LzValue *argv, LzValue *result) {
    LzCoroutine *co = (LzCoroutine *)argv[0].as.object;
    if (co->state != LZ_CO_SUSPENDED) {
        return lz_vm_set_error(vm, LZ_ERR_RUNTIME, "cannot resume a coroutine that is %s", lz_coroutine_status(co));
    }
    uintptr_t saved_floor = vm->c_stack_floor;
    LzStatus status = lz_vm_check_native_depth(vm);
    if (status != LZ_OK) {
        return status;
    }
    LzExecutionContext *caller = vm->context;
    LzCoroutine *parent = caller->coroutine;
    const struct LzNative *saved_native = vm->current_native;
    uint32_t call_site = vm->native_call_offset;
    unsigned saved_depth = vm->native_depth;
    bool saved_trace_fixed = vm->trace_fixed;
    lz_object_retain(&co->base);
    co->state = LZ_CO_RUNNING;
    if (parent != NULL) {
        parent->state = LZ_CO_NORMAL;
    }
    vm->native_depth++;
    co->yield_depth = vm->native_depth;
    bool first = co->context.stack == NULL;
    lz_vm_context_switch(vm, &co->context);
    size_t failed_at = 0;
    if (first) {
        status = lz_vm_context_reserve(vm, (size_t)argc, 2, 0);
        if (status == LZ_OK) {
            LzClosure *closure = lz_value_as_closure(&co->entry);
            /* The ordinary call machinery needs a caller frame. This sentinel never executes. */
            vm->frames[vm->frame_count++] = (LzCallFrame){.chunk = &closure->function->chunk};
            vm->stack[vm->stack_count++] = co->entry;
            lz_value_retain(&co->entry);
            for (int i = 1; i < argc; i++) {
                vm->stack[vm->stack_count++] = argv[i];
                lz_value_retain(&argv[i]);
            }
            bool entered = false;
            status = lz_vm_call(vm, (size_t)argc - 1, 0, LZ_NO_OFFSET, &entered);
        }
    } else {
        LzValue reply;
        status = pack(vm, (size_t)argc - 1, argv + 1, &reply);
        if (status == LZ_OK) {
            LzValue old = vm->stack[vm->stack_count - 1];
            vm->stack[vm->stack_count - 1] = reply;
            lz_value_release(&vm->heap, &old);
        }
    }
    if (status == LZ_OK) {
        status = lz_vm_run_until(vm, 1, &failed_at);
    }
    if (status == LZ_OK || status == LZ_YIELD) {
        *result = vm->stack[vm->stack_count - 1];
        vm->stack[vm->stack_count - 1] = lz_value_nil();
    } else if (!vm->trace_fixed && vm->frame_count > 1) {
        const LzChunk *chunk = vm->frames[vm->frame_count - 1].chunk;
        if (vm->error_offset == LZ_NO_OFFSET && failed_at < chunk->count) {
            vm->error_offset = chunk->offsets[failed_at];
            vm->error_source = lz_chunk_source(chunk);
        }
        lz_vm_capture_trace(vm);
    }
    lz_vm_context_switch(vm, caller);
    vm->current_native = saved_native;
    vm->native_call_offset = call_site;
    vm->native_depth = saved_depth;
    vm->c_stack_floor = saved_floor;
    if (parent != NULL) {
        parent->state = LZ_CO_RUNNING;
    }
    if (status == LZ_YIELD) {
        co->state = LZ_CO_SUSPENDED;
        vm->trace_fixed = saved_trace_fixed;
        status = LZ_OK;
    } else {
        co->state = status == LZ_OK ? LZ_CO_DONE : LZ_CO_FAILED;
        if (status != LZ_OK) {
            append_caller_trace(vm, call_site);
            vm->trace_fixed = true;
        } else {
            vm->trace_fixed = saved_trace_fixed;
        }
        lz_coroutine_clear(co);
    }
    lz_object_release(&vm->heap, &co->base);
    return status;
}
