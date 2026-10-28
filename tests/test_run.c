#include <stdint.h>
#include <string.h>

#include "chunk_helpers.h"
#include "lizard/builtin.h"
#include "test_util.h"

static LzStatus make_greeting(LzVM *vm, int argc, const LzValue *argv, LzValue *result) {
    (void)argc;
    (void)argv;
    LzString *text = lz_string_new(&vm->heap, "hi", 2);
    if (text == NULL) {
        return lz_vm_out_of_memory(vm);
    }
    *result = lz_value_object(&text->base);
    return LZ_OK;
}

static LzStatus needs_one(LzVM *vm, int argc, const LzValue *argv, LzValue *result) {
    (void)vm;
    (void)argc;
    (void)argv;
    *result = lz_value_nil();
    return LZ_OK;
}

static void define_native(LzVM *vm, const char *name, LzNativeFn fn, int min_args, int max_args) {
    LzString *key = lz_vm_intern(vm, name, strlen(name));
    LzNative *native = lz_native_new(&vm->heap, key, fn, min_args, max_args);
    (void)lz_vm_define_global(vm, key, lz_value_object(&native->base));
    lz_object_release(&vm->heap, &native->base);
}

static void setup(LzVM *vm, LzBuffer *out, LzBuffer *err) {
    memset(out, 0, sizeof *out);
    memset(err, 0, sizeof *err);
    lz_vm_init(vm, lz_writer_from_buffer(out), lz_writer_from_buffer(err));
    LZ_CHECK(lz_builtin_install(vm) == LZ_OK);
}

static void teardown(LzVM *vm, LzChunk *chunk, LzBuffer *out, LzBuffer *err) {
    lz_chunk_free(&vm->heap, chunk);
    lz_vm_release_roots(vm);
    LZ_CHECK(vm->heap.live_objects == 0);
    lz_vm_free(vm);
    lz_buffer_free(out);
    lz_buffer_free(err);
}

static bool output_is(const LzBuffer *out, const char *expected) {
    return out->data != NULL && strcmp(out->data, expected) == 0;
}

static void test_println_call(void) {
    LzVM vm;
    LzBuffer out;
    LzBuffer err;
    setup(&vm, &out, &err);

    LzChunk chunk;
    lz_chunk_init(&chunk);
    emit_u16(&chunk, LZ_OP_GET_GLOBAL, constant_name(&vm, &chunk, "println"), 1);
    emit_u16(&chunk, LZ_OP_CONSTANT, constant_text(&vm, &chunk, "halo"), 1);
    emit_u16(&chunk, LZ_OP_CONSTANT, constant_value(&chunk, lz_value_int(42)), 1);
    emit_u8(&chunk, LZ_OP_CALL, 2, 1);
    emit(&chunk, LZ_OP_POP, 1);
    emit(&chunk, LZ_OP_RETURN, 2);

    LZ_CHECK(lz_vm_run(&vm, &chunk) == LZ_OK);
    LZ_CHECK(output_is(&out, "halo 42\n"));
    LZ_CHECK(vm.stack_count == 0);

    teardown(&vm, &chunk, &out, &err);
}

static void test_globals(void) {
    LzVM vm;
    LzBuffer out;
    LzBuffer err;
    setup(&vm, &out, &err);

    uint16_t x = 0;
    LzChunk chunk;
    lz_chunk_init(&chunk);
    emit_u16(&chunk, LZ_OP_CONSTANT, constant_value(&chunk, lz_value_int(10)), 1);
    x = constant_name(&vm, &chunk, "x");
    emit_u16(&chunk, LZ_OP_DEFINE_GLOBAL, x, 1);
    emit_u16(&chunk, LZ_OP_GET_GLOBAL, constant_name(&vm, &chunk, "println"), 2);
    emit_u16(&chunk, LZ_OP_GET_GLOBAL, x, 2);
    emit_u8(&chunk, LZ_OP_CALL, 1, 2);
    emit(&chunk, LZ_OP_POP, 2);
    emit(&chunk, LZ_OP_RETURN, 3);

    LZ_CHECK(lz_vm_run(&vm, &chunk) == LZ_OK);
    LZ_CHECK(output_is(&out, "10\n"));

    teardown(&vm, &chunk, &out, &err);
}

static void test_native_result_is_owned(void) {
    LzVM vm;
    LzBuffer out;
    LzBuffer err;
    setup(&vm, &out, &err);
    define_native(&vm, "greet", make_greeting, 0, 0);

    LzChunk chunk;
    lz_chunk_init(&chunk);
    emit_u16(&chunk, LZ_OP_GET_GLOBAL, constant_name(&vm, &chunk, "println"), 1);
    emit_u16(&chunk, LZ_OP_GET_GLOBAL, constant_name(&vm, &chunk, "greet"), 1);
    emit_u8(&chunk, LZ_OP_CALL, 0, 1);
    emit_u8(&chunk, LZ_OP_CALL, 1, 1);
    emit(&chunk, LZ_OP_POP, 1);
    emit(&chunk, LZ_OP_RETURN, 1);

    LZ_CHECK(lz_vm_run(&vm, &chunk) == LZ_OK);
    LZ_CHECK(output_is(&out, "hi\n"));

    lz_chunk_free(&vm.heap, &chunk);
    lz_vm_release_roots(&vm);
    LZ_CHECK(vm.heap.live_objects == 0);
    lz_vm_free(&vm);
    lz_buffer_free(&out);
    lz_buffer_free(&err);
}

static void test_runtime_errors(void) {
    LzVM vm;
    LzBuffer out;
    LzBuffer err;
    setup(&vm, &out, &err);
    define_native(&vm, "one", needs_one, 1, 1);

    LzChunk undefined;
    lz_chunk_init(&undefined);
    emit_u16(&undefined, LZ_OP_GET_GLOBAL, constant_name(&vm, &undefined, "nope"), 7);
    emit(&undefined, LZ_OP_RETURN, 7);
    LZ_CHECK(lz_vm_run(&vm, &undefined) == LZ_ERR_RUNTIME);
    LZ_CHECK(strstr(vm.error, "undefined variable 'nope'") != NULL);
    LZ_CHECK(vm.error_offset == 7);
    LZ_CHECK(vm.stack_count == 0);

    LzChunk not_callable;
    lz_chunk_init(&not_callable);
    emit_u16(&not_callable, LZ_OP_CONSTANT, constant_value(&not_callable, lz_value_int(1)), 4);
    emit_u8(&not_callable, LZ_OP_CALL, 0, 4);
    emit(&not_callable, LZ_OP_RETURN, 4);
    LZ_CHECK(lz_vm_run(&vm, &not_callable) == LZ_ERR_RUNTIME);
    LZ_CHECK(strstr(vm.error, "can only call functions") != NULL);
    LZ_CHECK(vm.error_offset == 4);

    LzChunk arity;
    lz_chunk_init(&arity);
    emit_u16(&arity, LZ_OP_GET_GLOBAL, constant_name(&vm, &arity, "one"), 9);
    emit_u8(&arity, LZ_OP_CALL, 0, 9);
    emit(&arity, LZ_OP_RETURN, 9);
    LZ_CHECK(lz_vm_run(&vm, &arity) == LZ_ERR_ARITY);
    LZ_CHECK(strstr(vm.error, "one()") != NULL);
    LZ_CHECK(vm.error_offset == 9);
    LZ_CHECK(vm.stack_count == 0);

    lz_chunk_free(&vm.heap, &undefined);
    lz_chunk_free(&vm.heap, &not_callable);
    teardown(&vm, &arity, &out, &err);
}

static void test_malformed_bytecode(void) {
    LzVM vm;
    LzBuffer out;
    LzBuffer err;
    setup(&vm, &out, &err);

    LzChunk truncated;
    lz_chunk_init(&truncated);
    emit(&truncated, LZ_OP_CONSTANT, 1);
    LZ_CHECK(lz_vm_run(&vm, &truncated) == LZ_ERR_RUNTIME);
    LZ_CHECK(strstr(vm.error, "malformed bytecode") != NULL);

    LzChunk no_return;
    lz_chunk_init(&no_return);
    emit(&no_return, LZ_OP_NIL, 1);
    LZ_CHECK(lz_vm_run(&vm, &no_return) == LZ_ERR_RUNTIME);
    LZ_CHECK(vm.stack_count == 0);

    LzChunk underflow;
    lz_chunk_init(&underflow);
    emit(&underflow, LZ_OP_POP, 1);
    LZ_CHECK(lz_vm_run(&vm, &underflow) == LZ_ERR_RUNTIME);

    LzChunk bad_index;
    lz_chunk_init(&bad_index);
    emit_u16(&bad_index, LZ_OP_CONSTANT, 5, 1);
    LZ_CHECK(lz_vm_run(&vm, &bad_index) == LZ_ERR_RUNTIME);

    lz_chunk_free(&vm.heap, &truncated);
    lz_chunk_free(&vm.heap, &no_return);
    lz_chunk_free(&vm.heap, &underflow);
    teardown(&vm, &bad_index, &out, &err);
}

static void test_stack_overflow(void) {
    LzVM vm;
    LzBuffer out;
    LzBuffer err;
    setup(&vm, &out, &err);

    LzChunk chunk;
    lz_chunk_init(&chunk);
    for (int i = 0; i < LZ_STACK_MAX + 1; i++) {
        emit(&chunk, LZ_OP_NIL, 1);
    }
    emit(&chunk, LZ_OP_RETURN, 1);

    LZ_CHECK(lz_vm_run(&vm, &chunk) == LZ_ERR_STACK_OVERFLOW);
    LZ_CHECK(strstr(vm.error, "stack overflow") != NULL);
    LZ_CHECK(vm.stack_count == 0);

    teardown(&vm, &chunk, &out, &err);
}


static void test_arithmetic_program(void) {
    LzVM vm;
    LzBuffer out;
    LzBuffer err;
    setup(&vm, &out, &err);

    /* println(1 + 2 * 3, 7 / 2, -(5), 10 - 4) */
    LzChunk chunk;
    lz_chunk_init(&chunk);
    emit_u16(&chunk, LZ_OP_GET_GLOBAL, constant_name(&vm, &chunk, "println"), 1);
    emit_u16(&chunk, LZ_OP_CONSTANT, constant_value(&chunk, lz_value_int(1)), 1);
    emit_u16(&chunk, LZ_OP_CONSTANT, constant_value(&chunk, lz_value_int(2)), 1);
    emit_u16(&chunk, LZ_OP_CONSTANT, constant_value(&chunk, lz_value_int(3)), 1);
    emit(&chunk, LZ_OP_MULTIPLY, 1);
    emit(&chunk, LZ_OP_ADD, 1);
    emit_u16(&chunk, LZ_OP_CONSTANT, constant_value(&chunk, lz_value_int(7)), 1);
    emit_u16(&chunk, LZ_OP_CONSTANT, constant_value(&chunk, lz_value_int(2)), 1);
    emit(&chunk, LZ_OP_DIVIDE, 1);
    emit_u16(&chunk, LZ_OP_CONSTANT, constant_value(&chunk, lz_value_int(5)), 1);
    emit(&chunk, LZ_OP_NEGATE, 1);
    emit_u16(&chunk, LZ_OP_CONSTANT, constant_value(&chunk, lz_value_int(10)), 1);
    emit_u16(&chunk, LZ_OP_CONSTANT, constant_value(&chunk, lz_value_int(4)), 1);
    emit(&chunk, LZ_OP_SUBTRACT, 1);
    emit_u8(&chunk, LZ_OP_CALL, 4, 1);
    emit(&chunk, LZ_OP_POP, 1);
    emit(&chunk, LZ_OP_RETURN, 2);

    LZ_CHECK(lz_vm_run(&vm, &chunk) == LZ_OK);
    LZ_CHECK(output_is(&out, "7 3.5 -5 6\n"));
    LZ_CHECK(vm.stack_count == 0);

    teardown(&vm, &chunk, &out, &err);
}

static void test_arithmetic_errors(void) {
    LzVM vm;
    LzBuffer out;
    LzBuffer err;
    setup(&vm, &out, &err);

    LzChunk overflow;
    lz_chunk_init(&overflow);
    emit_u16(&overflow, LZ_OP_CONSTANT, constant_value(&overflow, lz_value_int(INT64_MAX)), 3);
    emit_u16(&overflow, LZ_OP_CONSTANT, constant_value(&overflow, lz_value_int(1)), 3);
    emit(&overflow, LZ_OP_ADD, 4);
    emit(&overflow, LZ_OP_RETURN, 4);
    LZ_CHECK(lz_vm_run(&vm, &overflow) == LZ_ERR_RUNTIME);
    LZ_CHECK(strstr(vm.error, "integer overflow") != NULL);
    LZ_CHECK(vm.error_offset == 4);
    LZ_CHECK(vm.stack_count == 0);

    LzChunk zero;
    lz_chunk_init(&zero);
    emit_u16(&zero, LZ_OP_CONSTANT, constant_value(&zero, lz_value_int(1)), 5);
    emit_u16(&zero, LZ_OP_CONSTANT, constant_value(&zero, lz_value_int(0)), 5);
    emit(&zero, LZ_OP_DIVIDE, 5);
    emit(&zero, LZ_OP_RETURN, 5);
    LZ_CHECK(lz_vm_run(&vm, &zero) == LZ_ERR_RUNTIME);
    LZ_CHECK(strstr(vm.error, "division by zero") != NULL);
    LZ_CHECK(vm.error_offset == 5);

    LzChunk wrong_type;
    lz_chunk_init(&wrong_type);
    emit(&wrong_type, LZ_OP_TRUE, 6);
    emit(&wrong_type, LZ_OP_NEGATE, 6);
    emit(&wrong_type, LZ_OP_RETURN, 6);
    LZ_CHECK(lz_vm_run(&vm, &wrong_type) == LZ_ERR_RUNTIME);
    LZ_CHECK(strstr(vm.error, "operand must be a number") != NULL);
    LZ_CHECK(vm.stack_count == 0);

    LzChunk underflow;
    lz_chunk_init(&underflow);
    emit(&underflow, LZ_OP_NIL, 7);
    emit(&underflow, LZ_OP_ADD, 7);
    LZ_CHECK(lz_vm_run(&vm, &underflow) == LZ_ERR_RUNTIME);
    LZ_CHECK(strstr(vm.error, "malformed bytecode") != NULL);
    LZ_CHECK(vm.stack_count == 0);

    lz_chunk_free(&vm.heap, &overflow);
    lz_chunk_free(&vm.heap, &zero);
    lz_chunk_free(&vm.heap, &wrong_type);
    teardown(&vm, &underflow, &out, &err);
}

static void test_comparison_program(void) {
    LzVM vm;
    LzBuffer out;
    LzBuffer err;
    setup(&vm, &out, &err);

    /* println(1 < 2, 2 > 3, 1 == 1.0, !nil, !0, "a" == "a", nil == false) */
    LzChunk chunk;
    lz_chunk_init(&chunk);
    emit_u16(&chunk, LZ_OP_GET_GLOBAL, constant_name(&vm, &chunk, "println"), 1);
    emit_u16(&chunk, LZ_OP_CONSTANT, constant_value(&chunk, lz_value_int(1)), 1);
    emit_u16(&chunk, LZ_OP_CONSTANT, constant_value(&chunk, lz_value_int(2)), 1);
    emit(&chunk, LZ_OP_LESS, 1);
    emit_u16(&chunk, LZ_OP_CONSTANT, constant_value(&chunk, lz_value_int(2)), 1);
    emit_u16(&chunk, LZ_OP_CONSTANT, constant_value(&chunk, lz_value_int(3)), 1);
    emit(&chunk, LZ_OP_GREATER, 1);
    emit_u16(&chunk, LZ_OP_CONSTANT, constant_value(&chunk, lz_value_int(1)), 1);
    emit_u16(&chunk, LZ_OP_CONSTANT, constant_value(&chunk, lz_value_float(1.0)), 1);
    emit(&chunk, LZ_OP_EQUAL, 1);
    emit(&chunk, LZ_OP_NIL, 1);
    emit(&chunk, LZ_OP_NOT, 1);
    emit_u16(&chunk, LZ_OP_CONSTANT, constant_value(&chunk, lz_value_int(0)), 1);
    emit(&chunk, LZ_OP_NOT, 1);
    emit_u16(&chunk, LZ_OP_CONSTANT, constant_text(&vm, &chunk, "a"), 1);
    emit_u16(&chunk, LZ_OP_CONSTANT, constant_text(&vm, &chunk, "a"), 1);
    emit(&chunk, LZ_OP_EQUAL, 1);
    emit(&chunk, LZ_OP_NIL, 1);
    emit(&chunk, LZ_OP_FALSE, 1);
    emit(&chunk, LZ_OP_EQUAL, 1);
    emit_u8(&chunk, LZ_OP_CALL, 7, 1);
    emit(&chunk, LZ_OP_POP, 1);
    emit(&chunk, LZ_OP_RETURN, 2);

    LZ_CHECK(lz_vm_run(&vm, &chunk) == LZ_OK);
    LZ_CHECK(output_is(&out, "true false true true false true false\n"));
    LZ_CHECK(vm.stack_count == 0);

    teardown(&vm, &chunk, &out, &err);
}

static void test_comparison_errors(void) {
    LzVM vm;
    LzBuffer out;
    LzBuffer err;
    setup(&vm, &out, &err);

    LzChunk text_order;
    lz_chunk_init(&text_order);
    emit_u16(&text_order, LZ_OP_CONSTANT, constant_text(&vm, &text_order, "a"), 8);
    emit_u16(&text_order, LZ_OP_CONSTANT, constant_text(&vm, &text_order, "b"), 8);
    emit(&text_order, LZ_OP_LESS, 9);
    emit(&text_order, LZ_OP_RETURN, 9);
    LZ_CHECK(lz_vm_run(&vm, &text_order) == LZ_ERR_RUNTIME);
    LZ_CHECK(strstr(vm.error, "operands must be numbers") != NULL);
    LZ_CHECK(vm.error_offset == 9);
    LZ_CHECK(vm.stack_count == 0);

    LzChunk underflow;
    lz_chunk_init(&underflow);
    emit(&underflow, LZ_OP_NIL, 1);
    emit(&underflow, LZ_OP_EQUAL, 1);
    LZ_CHECK(lz_vm_run(&vm, &underflow) == LZ_ERR_RUNTIME);
    LZ_CHECK(strstr(vm.error, "malformed bytecode") != NULL);

    LzChunk not_underflow;
    lz_chunk_init(&not_underflow);
    emit(&not_underflow, LZ_OP_NOT, 1);
    LZ_CHECK(lz_vm_run(&vm, &not_underflow) == LZ_ERR_RUNTIME);

    lz_chunk_free(&vm.heap, &text_order);
    lz_chunk_free(&vm.heap, &underflow);
    teardown(&vm, &not_underflow, &out, &err);
}

static void test_stage3_program(void) {
    LzVM vm;
    LzBuffer out;
    LzBuffer err;
    setup(&vm, &out, &err);

    /* println("foo" + "bar", 7 // 2, -7 % 3, "a" + "b" + "c") */
    LzChunk chunk;
    lz_chunk_init(&chunk);
    emit_u16(&chunk, LZ_OP_GET_GLOBAL, constant_name(&vm, &chunk, "println"), 1);
    emit_u16(&chunk, LZ_OP_CONSTANT, constant_text(&vm, &chunk, "foo"), 1);
    emit_u16(&chunk, LZ_OP_CONSTANT, constant_text(&vm, &chunk, "bar"), 1);
    emit(&chunk, LZ_OP_ADD, 1);
    emit_u16(&chunk, LZ_OP_CONSTANT, constant_value(&chunk, lz_value_int(7)), 1);
    emit_u16(&chunk, LZ_OP_CONSTANT, constant_value(&chunk, lz_value_int(2)), 1);
    emit(&chunk, LZ_OP_FLOOR_DIVIDE, 1);
    emit_u16(&chunk, LZ_OP_CONSTANT, constant_value(&chunk, lz_value_int(-7)), 1);
    emit_u16(&chunk, LZ_OP_CONSTANT, constant_value(&chunk, lz_value_int(3)), 1);
    emit(&chunk, LZ_OP_MODULO, 1);
    emit_u16(&chunk, LZ_OP_CONSTANT, constant_text(&vm, &chunk, "a"), 1);
    emit_u16(&chunk, LZ_OP_CONSTANT, constant_text(&vm, &chunk, "b"), 1);
    emit(&chunk, LZ_OP_ADD, 1);
    emit_u16(&chunk, LZ_OP_CONSTANT, constant_text(&vm, &chunk, "c"), 1);
    emit(&chunk, LZ_OP_ADD, 1);
    emit_u8(&chunk, LZ_OP_CALL, 4, 1);
    emit(&chunk, LZ_OP_POP, 1);
    emit(&chunk, LZ_OP_RETURN, 2);

    LZ_CHECK(lz_vm_run(&vm, &chunk) == LZ_OK);
    LZ_CHECK(output_is(&out, "foobar 3 2 abc\n"));
    LZ_CHECK(vm.stack_count == 0);

    teardown(&vm, &chunk, &out, &err);
}

static void test_stage3_errors(void) {
    LzVM vm;
    LzBuffer out;
    LzBuffer err;
    setup(&vm, &out, &err);

    LzChunk mixed;
    lz_chunk_init(&mixed);
    emit_u16(&mixed, LZ_OP_CONSTANT, constant_text(&vm, &mixed, "a"), 3);
    emit_u16(&mixed, LZ_OP_CONSTANT, constant_value(&mixed, lz_value_int(1)), 3);
    emit(&mixed, LZ_OP_ADD, 4);
    emit(&mixed, LZ_OP_RETURN, 4);
    LZ_CHECK(lz_vm_run(&vm, &mixed) == LZ_ERR_RUNTIME);
    LZ_CHECK(strstr(vm.error, "two numbers or two strings") != NULL);
    LZ_CHECK(vm.error_offset == 4);
    LZ_CHECK(vm.stack_count == 0);

    LzChunk zero;
    lz_chunk_init(&zero);
    emit_u16(&zero, LZ_OP_CONSTANT, constant_value(&zero, lz_value_int(5)), 6);
    emit_u16(&zero, LZ_OP_CONSTANT, constant_value(&zero, lz_value_int(0)), 6);
    emit(&zero, LZ_OP_MODULO, 6);
    emit(&zero, LZ_OP_RETURN, 6);
    LZ_CHECK(lz_vm_run(&vm, &zero) == LZ_ERR_RUNTIME);
    LZ_CHECK(strstr(vm.error, "division by zero") != NULL);
    LZ_CHECK(vm.error_offset == 6);

    lz_chunk_free(&vm.heap, &mixed);
    teardown(&vm, &zero, &out, &err);
}

static uint16_t add_type(LzChunk *chunk, const LzType *type) {
    uint16_t index = 0;
    LZ_CHECK(lz_chunk_add_type(chunk, type, &index));
    return index;
}

static void test_check_type(void) {
    LzVM vm;
    LzBuffer out;
    LzBuffer err;
    setup(&vm, &out, &err);

    LzType number = lz_type_of_kinds(LZ_KINDS_NUMBER);
    LzType text = lz_type_of_kinds(LZ_KIND_STRING);

    LzChunk passes;
    lz_chunk_init(&passes);
    emit_u16(&passes, LZ_OP_CONSTANT, constant_value(&passes, lz_value_int(5)), 1);
    emit_u16(&passes, LZ_OP_CHECK_TYPE, add_type(&passes, &number), 1);
    emit(&passes, LZ_OP_POP, 1);
    emit(&passes, LZ_OP_RETURN, 1);
    LZ_CHECK(lz_vm_run(&vm, &passes) == LZ_OK);

    LzChunk fails;
    lz_chunk_init(&fails);
    emit_u16(&fails, LZ_OP_CONSTANT, constant_value(&fails, lz_value_int(5)), 4);
    emit_u16(&fails, LZ_OP_CHECK_TYPE, add_type(&fails, &text), 6);
    emit(&fails, LZ_OP_RETURN, 6);
    LZ_CHECK(lz_vm_run(&vm, &fails) == LZ_ERR_TYPE);
    LZ_CHECK(strstr(vm.error, "type error: expected string, got int") != NULL);
    LZ_CHECK(vm.error_offset == 6);
    LZ_CHECK(vm.stack_count == 0);

    LzString *r = lz_vm_intern(&vm, "r", 1);
    LzString *w = lz_vm_intern(&vm, "w", 1);
    LzType mode = lz_type_of_kinds(0);
    LzLiteral r_literal = {.kind = LZ_VAL_OBJECT, .as.string = r};
    LzLiteral w_literal = {.kind = LZ_VAL_OBJECT, .as.string = w};
    LZ_CHECK(lz_type_add_literal(&mode, &r_literal) && lz_type_add_literal(&mode, &w_literal));

    LzChunk literal_ok;
    lz_chunk_init(&literal_ok);
    emit_u16(&literal_ok, LZ_OP_CONSTANT, constant_text(&vm, &literal_ok, "w"), 1);
    emit_u16(&literal_ok, LZ_OP_CHECK_TYPE, add_type(&literal_ok, &mode), 1);
    emit(&literal_ok, LZ_OP_RETURN, 1);
    LZ_CHECK(lz_vm_run(&vm, &literal_ok) == LZ_OK);

    LzChunk literal_bad;
    lz_chunk_init(&literal_bad);
    emit_u16(&literal_bad, LZ_OP_CONSTANT, constant_text(&vm, &literal_bad, "x"), 1);
    emit_u16(&literal_bad, LZ_OP_CHECK_TYPE, add_type(&literal_bad, &mode), 1);
    emit(&literal_bad, LZ_OP_RETURN, 1);
    LZ_CHECK(lz_vm_run(&vm, &literal_bad) == LZ_ERR_TYPE);
    LZ_CHECK(strstr(vm.error, "expected \"r\" | \"w\", got \"x\"") != NULL);

    LzChunk bad_index;
    lz_chunk_init(&bad_index);
    emit(&bad_index, LZ_OP_NIL, 1);
    emit_u16(&bad_index, LZ_OP_CHECK_TYPE, 3, 1);
    LZ_CHECK(lz_vm_run(&vm, &bad_index) == LZ_ERR_RUNTIME);
    LZ_CHECK(strstr(vm.error, "type index out of range") != NULL);

    LzChunk empty_stack;
    lz_chunk_init(&empty_stack);
    emit_u16(&empty_stack, LZ_OP_CHECK_TYPE, add_type(&empty_stack, &text), 1);
    LZ_CHECK(lz_vm_run(&vm, &empty_stack) == LZ_ERR_RUNTIME);
    LZ_CHECK(strstr(vm.error, "malformed bytecode") != NULL);

    LzChunk truncated;
    lz_chunk_init(&truncated);
    emit(&truncated, LZ_OP_CHECK_TYPE, 1);
    LZ_CHECK(lz_vm_run(&vm, &truncated) == LZ_ERR_RUNTIME);

    lz_chunk_free(&vm.heap, &passes);
    lz_chunk_free(&vm.heap, &fails);
    lz_chunk_free(&vm.heap, &literal_ok);
    lz_chunk_free(&vm.heap, &literal_bad);
    lz_chunk_free(&vm.heap, &bad_index);
    lz_chunk_free(&vm.heap, &empty_stack);
    teardown(&vm, &truncated, &out, &err);
}

int main(void) {
    test_check_type();
    test_println_call();
    test_globals();
    test_native_result_is_owned();
    test_runtime_errors();
    test_malformed_bytecode();
    test_stack_overflow();
    test_arithmetic_program();
    test_arithmetic_errors();
    test_comparison_program();
    test_comparison_errors();
    test_stage3_program();
    test_stage3_errors();
    return LZ_TEST_RESULT();
}
