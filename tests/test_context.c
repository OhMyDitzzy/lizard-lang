#include <string.h>

#include "lizard/builtin.h"
#include "lizard/compiler.h"
#include "lizard/vm.h"
#include "test_util.h"

static void finish(LzVM *vm, LzBuffer *output) {
    lz_vm_release_roots(vm);
    LZ_CHECK(vm->heap.live_objects == 0);
    LZ_CHECK(vm->heap.live_bytes == 0);
    lz_vm_free(vm);
    lz_buffer_free(output);
}

static void test_fixed_storage(void) {
    LzBuffer output = {0};
    LzVM vm;
    lz_vm_init(&vm, lz_writer_from_buffer(&output), lz_writer_from_buffer(&output));
    LzExecutionContext context;
    lz_vm_context_init(&context);
    context.stack_limit = 16;
    context.frame_limit = 4;
    context.handler_limit = 2;
    LZ_CHECK(vm.stack == vm.main_stack && vm.stack_cap == LZ_STACK_MAX);
    LZ_CHECK(vm.frames == vm.main_frames && vm.frame_cap == LZ_FRAMES_MAX);
    LZ_CHECK(vm.handlers == vm.main_handlers && vm.handler_cap == LZ_HANDLERS_MAX);

    vm.stack[vm.stack_count++] = lz_value_int(17);
    LzValue *main_stack = vm.stack;
    lz_vm_context_switch(&vm, &context);
    LZ_CHECK(vm.stack == NULL && vm.stack_cap == 16);
    LZ_CHECK(vm.frame_cap == 4 && vm.handler_cap == 2);
    size_t bytes = 16 * sizeof(LzValue) + 4 * sizeof(LzCallFrame) + 2 * sizeof(LzHandler);
    LZ_CHECK(lz_vm_context_reserve(&vm, 1, 1, 1) == LZ_OK);
    LZ_CHECK(vm.heap.live_bytes == bytes);
    LZ_CHECK(context.stack_capacity == 16 && context.frame_capacity == 4 && context.handler_capacity == 2);
    LzValue *fixed_stack = vm.stack;
    vm.stack[vm.stack_count++] = lz_value_int(23);
    LzUpvalue *upvalue = lz_upvalue_new(&vm.heap, &vm.stack[0]);
    LZ_CHECK(upvalue != NULL);
    lz_object_retain(&upvalue->base);
    vm.open_upvalues = upvalue;
    LZ_CHECK(lz_vm_context_reserve(&vm, 16, 4, 2) == LZ_OK);
    LZ_CHECK(vm.stack == fixed_stack && upvalue->location == &vm.stack[0]);
    LZ_CHECK(lz_vm_context_reserve(&vm, 17, 0, 0) == LZ_ERR_STACK_OVERFLOW);
    LZ_CHECK(strcmp(vm.error, "stack overflow") == 0);
    LZ_CHECK(lz_vm_context_reserve(&vm, 0, 5, 0) == LZ_ERR_STACK_OVERFLOW);
    LZ_CHECK(strstr(vm.error, "too many nested calls") != NULL);
    LZ_CHECK(lz_vm_context_reserve(&vm, 0, 0, 3) == LZ_ERR_STACK_OVERFLOW);
    LZ_CHECK(strcmp(vm.error, "stack overflow: too many open try blocks") == 0);

    lz_vm_context_switch(&vm, &vm.main_context);
    LZ_CHECK(vm.stack == main_stack && vm.stack_count == 1 && vm.stack[0].as.integer == 17);
    LZ_CHECK(vm.stack_cap == LZ_STACK_MAX && vm.frame_cap == LZ_FRAMES_MAX);
    lz_vm_context_free(&vm, &context);
    LZ_CHECK(upvalue->location == &upvalue->closed && upvalue->closed.as.integer == 23);
    LZ_CHECK(vm.heap.live_bytes == upvalue->base.size);
    lz_object_release(&vm.heap, &upvalue->base);
    LZ_CHECK(vm.heap.live_bytes == 0);
    vm.stack_count = 0;
    finish(&vm, &output);
}

static LzStatus run_in_small_context(const char *code, size_t stack_limit, size_t frame_limit,
                                     size_t handler_limit, LzVM *vm, LzBuffer *output) {
    LzSource source;
    lz_source_from_string(&source, "context.lz", code);
    LzChunk chunk;
    LzDiagnostics diagnostics;
    lz_chunk_init(&chunk);
    LzStatus status = lz_compile(vm, &source, &chunk, &diagnostics);
    if (status != LZ_OK) {
        lz_chunk_free(&vm->heap, &chunk);
        return status;
    }

    LzExecutionContext context;
    lz_vm_context_init(&context);
    context.stack_limit = stack_limit;
    context.frame_limit = frame_limit;
    context.handler_limit = handler_limit;
    lz_vm_context_switch(vm, &context);
    status = lz_vm_context_reserve(vm, 1, 1, 0);
    if (status == LZ_OK) {
        status = lz_vm_run(vm, &chunk);
    }
    lz_vm_context_switch(vm, &vm->main_context);
    lz_vm_context_free(vm, &context);
    lz_chunk_free(&vm->heap, &chunk);
    (void)output;
    return status;
}

static void test_context_limits(void) {
    LzBuffer output = {0};
    LzVM vm;
    lz_vm_init(&vm, lz_writer_from_buffer(&output), lz_writer_from_buffer(&output));
    LZ_CHECK(lz_builtin_install(&vm) == LZ_OK);
    LZ_CHECK(run_in_small_context("fn recur(n) { return recur(n + 1) }\nrecur(0)\n", 256, 4, 8,
                                  &vm, &output) == LZ_ERR_STACK_OVERFLOW);
    LZ_CHECK(strstr(vm.error, "too many nested calls (limit 4)") != NULL);
    LZ_CHECK(run_in_small_context(
                 "fn recur(n) { let a = n\nlet b = n\nlet c = n\nlet d = n\nlet e = n\nlet f = n\nreturn recur(n + 1) }\nrecur(0)\n",
                 16, 100, 8, &vm, &output) == LZ_ERR_STACK_OVERFLOW);
    LZ_CHECK(strcmp(vm.error, "stack overflow") == 0);
    LZ_CHECK(run_in_small_context(
                 "fn recur(n) { try { return recur(n + 1) } catch e { return nil } }\nrecur(0)\n",
                 256, 100, 2, &vm, &output) == LZ_ERR_STACK_OVERFLOW);
    LZ_CHECK(strcmp(vm.error, "stack overflow: too many open try blocks") == 0);
    finish(&vm, &output);
}

static void test_heap_limit(void) {
    LzBuffer output = {0};
    LzVM vm;
    lz_vm_init(&vm, lz_writer_from_buffer(&output), lz_writer_from_buffer(&output));
    LzExecutionContext context;
    lz_vm_context_init(&context);
    context.stack_limit = 16;
    context.frame_limit = 4;
    context.handler_limit = 2;
    lz_vm_context_switch(&vm, &context);
    size_t bytes = 16 * sizeof(LzValue) + 4 * sizeof(LzCallFrame) + 2 * sizeof(LzHandler);
    for (size_t limit = 0; limit < bytes; limit += 16 * sizeof(LzValue)) {
        lz_vm_set_heap_limit(&vm, limit);
        LZ_CHECK(lz_vm_context_reserve(&vm, 1, 1, 1) == LZ_ERR_HEAP_LIMIT);
        LZ_CHECK(vm.heap.live_bytes == 0 && vm.stack == NULL && vm.frames == NULL);
    }
    lz_vm_set_heap_limit(&vm, bytes);
    LZ_CHECK(lz_vm_context_reserve(&vm, 1, 1, 1) == LZ_OK);
    LZ_CHECK(vm.heap.live_bytes == bytes);
    lz_vm_context_switch(&vm, &vm.main_context);
    lz_vm_context_free(&vm, &context);
    LZ_CHECK(vm.heap.live_bytes == 0);
    finish(&vm, &output);
}

int main(void) {
    test_fixed_storage();
    test_context_limits();
    test_heap_limit();
    return LZ_TEST_RESULT();
}
