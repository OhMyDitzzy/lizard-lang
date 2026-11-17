#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "lizard/vm.h"

void lz_vm_context_init(LzExecutionContext *context) {
    *context = (LzExecutionContext){
        .stack_limit = LZ_CO_STACK_MAX,
        .frame_limit = LZ_CO_FRAMES_MAX,
        .handler_limit = LZ_CO_HANDLERS_MAX,
    };
}

static void save_active(LzVM *vm) {
    LzExecutionContext *context = vm->context;
    context->stack = vm->stack;
    context->stack_count = vm->stack_count;
    context->frames = vm->frames;
    context->frame_count = vm->frame_count;
    context->handlers = vm->handlers;
    context->handler_count = vm->handler_count;
    context->open_upvalues = vm->open_upvalues;
}

void lz_vm_context_switch(LzVM *vm, LzExecutionContext *next) {
    save_active(vm);
    vm->context = next;
    vm->stack = next->stack;
    vm->stack_count = next->stack_count;
    vm->frames = next->frames;
    vm->frame_count = next->frame_count;
    vm->handlers = next->handlers;
    vm->stack_cap = next->stack_limit;
    vm->frame_cap = next->frame_limit;
    vm->handler_cap = next->handler_limit;
    vm->handler_count = next->handler_count;
    vm->open_upvalues = next->open_upvalues;
}

static LzStatus context_limit_error(LzVM *vm, size_t values, size_t frames, size_t handlers) {
    if (values > vm->stack_cap) {
        return lz_vm_set_error(vm, LZ_ERR_STACK_OVERFLOW, "stack overflow");
    }
    if (frames > vm->frame_cap) {
        LzStatus status = lz_vm_set_error(vm, LZ_ERR_STACK_OVERFLOW,
                                          "stack overflow: too many nested calls (limit %zu)", vm->frame_cap);
        lz_vm_set_help(vm, "a function calls itself without a base case, or the recursion is too deep (limit %zu)",
                       vm->frame_cap);
        return status;
    }
    if (handlers > vm->handler_cap) {
        return lz_vm_set_error(vm, LZ_ERR_STACK_OVERFLOW, "stack overflow: too many open try blocks");
    }
    return LZ_OK;
}

LzStatus lz_vm_context_reserve(LzVM *vm, size_t values, size_t frames, size_t handlers) {
    LzStatus status = context_limit_error(vm, values, frames, handlers);
    if (status != LZ_OK) {
        return status;
    }
    LzExecutionContext *context = vm->context;
    if (context->embedded || context->stack != NULL) {
        return LZ_OK;
    }

    size_t stack_bytes = context->stack_limit * sizeof *context->stack;
    size_t frame_bytes = context->frame_limit * sizeof *context->frames;
    size_t handler_bytes = context->handler_limit * sizeof *context->handlers;
    size_t charged = 0;
    if (!lz_heap_charge(&vm->heap, stack_bytes)) {
        return lz_vm_out_of_memory(vm);
    }
    charged += stack_bytes;
    LzValue *stack = malloc(stack_bytes);
    LzCallFrame *frame_buffer = NULL;
    LzHandler *handler_buffer = NULL;
    if (stack != NULL && lz_heap_charge(&vm->heap, frame_bytes)) {
        charged += frame_bytes;
        frame_buffer = malloc(frame_bytes);
    }
    if (frame_buffer != NULL && lz_heap_charge(&vm->heap, handler_bytes)) {
        charged += handler_bytes;
        handler_buffer = malloc(handler_bytes);
    }
    if (stack == NULL || frame_buffer == NULL || handler_buffer == NULL) {
        lz_free_sized(stack, stack_bytes);
        lz_free_sized(frame_buffer, frame_bytes);
        lz_free_sized(handler_buffer, handler_bytes);
        lz_heap_uncharge(&vm->heap, charged);
        return lz_vm_out_of_memory(vm);
    }
#ifdef LZ_POISON
    memset(stack, 0xA5, stack_bytes);
    memset(frame_buffer, 0xA5, frame_bytes);
    memset(handler_buffer, 0xA5, handler_bytes);
#endif
    context->stack = vm->stack = stack;
    context->stack_capacity = context->stack_limit;
    context->frames = vm->frames = frame_buffer;
    context->frame_capacity = context->frame_limit;
    context->handlers = vm->handlers = handler_buffer;
    context->handler_capacity = context->handler_limit;
    save_active(vm);
    return LZ_OK;
}

void lz_vm_context_free(LzVM *vm, LzExecutionContext *context) {
    assert(context != vm->context && !context->embedded);
    while (context->open_upvalues != NULL) {
        LzUpvalue *upvalue = context->open_upvalues;
        context->open_upvalues = upvalue->next_open;
        lz_value_retain(upvalue->location);
        upvalue->closed = *upvalue->location;
        upvalue->location = &upvalue->closed;
        upvalue->next_open = NULL;
        if (!vm->skink.collecting) {
            lz_skink_note_closed(vm, upvalue);
        }
        lz_object_release(&vm->heap, &upvalue->base);
    }
    /* Frame closures borrow their callee slots. Releasing them again here would drop a reference that this context never owned. */
    context->frame_count = 0;
    context->handler_count = 0;
    while (context->stack_count != 0) {
        lz_value_release(&vm->heap, &context->stack[--context->stack_count]);
    }
    if (!vm->skink.collecting) {
        lz_skink_flush(vm);
    }
    size_t stack_bytes = context->stack_capacity * sizeof *context->stack;
    size_t frame_bytes = context->frame_capacity * sizeof *context->frames;
    size_t handler_bytes = context->handler_capacity * sizeof *context->handlers;
    lz_free_sized(context->stack, stack_bytes);
    lz_free_sized(context->frames, frame_bytes);
    lz_free_sized(context->handlers, handler_bytes);
    lz_heap_uncharge(&vm->heap, stack_bytes + frame_bytes + handler_bytes);
    lz_vm_context_init(context);
}
