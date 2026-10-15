#include <stdint.h>
#include <string.h>

#include "chunk_helpers.h"
#include "lizard/builtin.h"
#include "test_util.h"

static LzStatus spend_hundred(LzVM *vm, int argc, const LzValue *argv, LzValue *result) {
    (void)argc;
    (void)argv;
    LzStatus status = lz_vm_charge_fuel(vm, 100);
    if (status != LZ_OK) {
        return status;
    }
    *result = lz_value_nil();
    return LZ_OK;
}

static void setup(LzVM *vm, LzBuffer *out) {
    memset(out, 0, sizeof *out);
    lz_vm_init(vm, lz_writer_from_buffer(out), lz_writer_from_buffer(out));
    LZ_CHECK(lz_builtin_install(vm) == LZ_OK);
}

static void teardown(LzVM *vm, LzChunk *chunk, LzBuffer *out) {
    lz_chunk_free(&vm->heap, chunk);
    lz_vm_release_roots(vm);
    LZ_CHECK(vm->heap.live_objects == 0);
    lz_vm_free(vm);
    lz_buffer_free(out);
}

static void test_heap_limit_boundary(void) {
    LzBuffer out = {0};
    LzVM vm;
    lz_vm_init(&vm, lz_writer_from_buffer(&out), lz_writer_from_buffer(&out));

    size_t exact = sizeof(LzString) + 4;
    lz_vm_set_heap_limit(&vm, exact);

    LzString *first = lz_string_new(&vm.heap, "abc", 3);
    LZ_CHECK(first != NULL);
    LZ_CHECK(!vm.heap.limit_hit);

    LZ_CHECK(lz_string_new(&vm.heap, "x", 1) == NULL);
    LZ_CHECK(vm.heap.limit_hit);
    LZ_CHECK(lz_vm_out_of_memory(&vm) == LZ_ERR_HEAP_LIMIT);
    LZ_CHECK(strstr(vm.error, "heap limit") != NULL);
    LZ_CHECK(lz_vm_out_of_memory(&vm) == LZ_ERR_MEMORY);

    lz_object_release(&vm.heap, &first->base);
    LzString *again = lz_string_new(&vm.heap, "abc", 3);
    LZ_CHECK(again != NULL);

    lz_vm_set_heap_limit(&vm, 1);
    LZ_CHECK(lz_string_new(&vm.heap, "", 0) == NULL);
    LZ_CHECK(vm.heap.live_objects == 1);

    lz_vm_set_heap_limit(&vm, LZ_HEAP_UNLIMITED);
    LzString *free_again = lz_string_new(&vm.heap, "fine", 4);
    LZ_CHECK(free_again != NULL);

    lz_object_release(&vm.heap, &again->base);
    lz_object_release(&vm.heap, &free_again->base);
    lz_vm_free(&vm);
    lz_buffer_free(&out);
}

static void test_heap_limit_in_a_program(void) {
    LzVM vm;
    LzBuffer out;
    setup(&vm, &out);

    LzChunk chunk;
    lz_chunk_init(&chunk);
    emit_u16(&chunk, LZ_OP_CONSTANT, constant_text(&vm, &chunk, "ab"), 3);
    emit_u16(&chunk, LZ_OP_CONSTANT, constant_text(&vm, &chunk, "cd"), 3);
    emit(&chunk, LZ_OP_ADD, 4);
    emit(&chunk, LZ_OP_POP, 4);
    emit(&chunk, LZ_OP_RETURN, 5);

    lz_vm_set_heap_limit(&vm, vm.heap.live_bytes);
    LZ_CHECK(lz_vm_run(&vm, &chunk) == LZ_ERR_HEAP_LIMIT);
    LZ_CHECK(strstr(vm.error, "heap limit") != NULL);
    LZ_CHECK(vm.error_offset == 4);
    LZ_CHECK(vm.stack_count == 0);

    lz_vm_set_heap_limit(&vm, LZ_HEAP_UNLIMITED);
    LZ_CHECK(lz_vm_run(&vm, &chunk) == LZ_OK);

    teardown(&vm, &chunk, &out);
}

static void build_five_instructions(LzChunk *chunk) {
    emit(chunk, LZ_OP_NIL, 1);
    emit(chunk, LZ_OP_POP, 2);
    emit(chunk, LZ_OP_NIL, 3);
    emit(chunk, LZ_OP_POP, 4);
    emit(chunk, LZ_OP_RETURN, 5);
}

static void test_fuel(void) {
    LzVM vm;
    LzBuffer out;
    setup(&vm, &out);

    LzChunk chunk;
    lz_chunk_init(&chunk);
    build_five_instructions(&chunk);

    LZ_CHECK(lz_vm_run(&vm, &chunk) == LZ_OK);
    LZ_CHECK(vm.instructions == 5);
    LZ_CHECK(vm.fuel == LZ_FUEL_UNLIMITED);

    lz_vm_set_fuel(&vm, 5);
    LZ_CHECK(lz_vm_run(&vm, &chunk) == LZ_OK);
    LZ_CHECK(vm.fuel == 0);
    LZ_CHECK(vm.instructions == 10);

    lz_vm_set_fuel(&vm, 4);
    LZ_CHECK(lz_vm_run(&vm, &chunk) == LZ_ERR_FUEL);
    LZ_CHECK(strstr(vm.error, "instruction limit") != NULL);
    LZ_CHECK(vm.error_offset == 5);
    LZ_CHECK(vm.stack_count == 0);

    lz_vm_set_fuel(&vm, 0);
    LZ_CHECK(lz_vm_run(&vm, &chunk) == LZ_ERR_FUEL);
    LZ_CHECK(vm.error_offset == 1);

    lz_vm_set_fuel(&vm, 7);
    LZ_CHECK(lz_vm_run(&vm, &chunk) == LZ_OK);
    LZ_CHECK(vm.fuel == 2);
    LZ_CHECK(lz_vm_run(&vm, &chunk) == LZ_ERR_FUEL);

    teardown(&vm, &chunk, &out);
}

static void test_natives_can_charge_fuel(void) {
    LzVM vm;
    LzBuffer out;
    setup(&vm, &out);

    LzString *name = lz_vm_intern(&vm, "heavy", 5);
    LzNative *heavy = lz_native_new(&vm.heap, name, spend_hundred, 0, 0);
    LZ_CHECK(lz_vm_define_global(&vm, name, lz_value_object(&heavy->base)));
    lz_object_release(&vm.heap, &heavy->base);

    LzChunk chunk;
    lz_chunk_init(&chunk);
    emit_u16(&chunk, LZ_OP_GET_GLOBAL, constant_name(&vm, &chunk, "heavy"), 1);
    emit_u8(&chunk, LZ_OP_CALL, 0, 1);
    emit(&chunk, LZ_OP_POP, 1);
    emit(&chunk, LZ_OP_RETURN, 2);

    lz_vm_set_fuel(&vm, 50);
    LZ_CHECK(lz_vm_run(&vm, &chunk) == LZ_ERR_FUEL);
    LZ_CHECK(vm.error_offset == 1);
    LZ_CHECK(vm.fuel == 0);

    lz_vm_set_fuel(&vm, 103);
    LZ_CHECK(lz_vm_run(&vm, &chunk) == LZ_ERR_FUEL);
    LZ_CHECK(vm.error_offset == 2);

    lz_vm_set_fuel(&vm, 104);
    LZ_CHECK(lz_vm_run(&vm, &chunk) == LZ_OK);
    LZ_CHECK(vm.fuel == 0);

    teardown(&vm, &chunk, &out);
}

int main(void) {
    test_heap_limit_boundary();
    test_heap_limit_in_a_program();
    test_fuel();
    test_natives_can_charge_fuel();
    return LZ_TEST_RESULT();
}
