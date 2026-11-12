#include <stdint.h>
#include <string.h>

#include "chunk_helpers.h"
#include <math.h>

#include "lizard/arith.h"
#include "lizard/builtin.h"
#include "lizard/compare.h"
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


/*
 * The interpreter's fast path for + - * and the comparisons must give what the general functions
 * give, for the same values: same result bits, same error, same status.
 */
static void run_pair(LzOpCode op, LzValue a, LzValue b) {
    LzBuffer out;
    LzBuffer err;
    LzVM vm;
    setup(&vm, &out, &err);

    LzValue expected = lz_value_nil();
    LzStatus expected_status;
    bool is_order = op == LZ_OP_LESS || op == LZ_OP_GREATER || op == LZ_OP_LESS_EQUAL ||
                    op == LZ_OP_GREATER_EQUAL;
    if (op == LZ_OP_EQUAL) {
        bool same = false;
        expected_status = lz_vm_values_equal(&vm, &a, &b, &same);
        expected = lz_value_bool(same);
    } else if (is_order) {
        expected_status = lz_compare_order(&vm, op, &a, &b, &expected);
    } else {
        expected_status = lz_arith_binary(&vm, op, &a, &b, &expected);
    }
    char expected_error[sizeof vm.error];
    memcpy(expected_error, vm.error, sizeof expected_error);

    LzChunk chunk;
    lz_chunk_init(&chunk);
    uint16_t name = constant_name(&vm, &chunk, "r");
    emit_u16(&chunk, LZ_OP_CONSTANT, constant_value(&chunk, a), 1);
    emit_u16(&chunk, LZ_OP_CONSTANT, constant_value(&chunk, b), 1);
    emit(&chunk, op, 1);
    emit_u16(&chunk, LZ_OP_DEFINE_GLOBAL, name, 1);
    emit(&chunk, LZ_OP_RETURN, 1);

    LzStatus status = lz_vm_run(&vm, &chunk);
    LZ_CHECK(status == expected_status);
    if (expected_status != LZ_OK) {
        LZ_CHECK(strcmp(vm.error, expected_error) == 0);
    } else {
        const LzValue *got = lz_vm_lookup(&vm, lz_vm_intern(&vm, "r", 1));
        LZ_CHECK(got != NULL && got->type == expected.type);
        if (got != NULL && got->type == expected.type) {
            if (got->type == LZ_VAL_FLOAT) {
                LZ_CHECK(memcmp(&got->as.number, &expected.as.number, sizeof(double)) == 0);
            } else if (got->type == LZ_VAL_INT) {
                LZ_CHECK(got->as.integer == expected.as.integer);
            } else {
                LZ_CHECK(got->as.boolean == expected.as.boolean);
            }
        }
    }
    teardown(&vm, &chunk, &out, &err);
}

static void test_fast_path_matches_general(void) {
    const LzValue values[] = {
        lz_value_int(0),         lz_value_int(1),          lz_value_int(-1),
        lz_value_int(2),         lz_value_int(INT64_MAX),  lz_value_int(INT64_MIN),
        lz_value_int(INT64_MAX - 1), lz_value_int(INT64_MIN + 1), lz_value_int(3037000500),
        lz_value_int(-3037000500), lz_value_int(9007199254740993),
        lz_value_float(0.0),     lz_value_float(-0.0),     lz_value_float(1.5),
        lz_value_float(-2.25),   lz_value_float(1e308),    lz_value_float(-1e308),
        lz_value_float(5e-324),  lz_value_float(HUGE_VAL), lz_value_float(-HUGE_VAL),
        lz_value_float(NAN),     lz_value_float(9007199254740992.0),
        lz_value_float(9223372036854775808.0),
        lz_value_bool(true),     lz_value_nil(),
    };
    const LzOpCode ops[] = {LZ_OP_ADD,  LZ_OP_SUBTRACT,    LZ_OP_MULTIPLY,
                            LZ_OP_LESS, LZ_OP_GREATER,     LZ_OP_LESS_EQUAL,
                            LZ_OP_GREATER_EQUAL, LZ_OP_EQUAL};
    size_t count = sizeof values / sizeof values[0];
    for (size_t o = 0; o < sizeof ops / sizeof ops[0]; o++) {
        for (size_t i = 0; i < count; i++) {
            for (size_t j = 0; j < count; j++) {
                run_pair(ops[o], values[i], values[j]);
            }
        }
    }
}

/* The primitive shortcut of the type check must agree with lz_type_matches on every pairing. */
static void test_check_type_shortcut_agrees(void) {
    LzBuffer out;
    LzBuffer err;
    LzVM vm;
    setup(&vm, &out, &err);
    LzString *text = lz_vm_intern(&vm, "abc", 3);
    LzString *other = lz_vm_intern(&vm, "xyz", 3);

    LzType types[16];
    size_t type_count = 0;
    const uint16_t kind_sets[] = {LZ_KIND_NIL, LZ_KIND_BOOL, LZ_KIND_INT, LZ_KIND_FLOAT, LZ_KIND_STRING,
                                  LZ_KIND_INT | LZ_KIND_NIL, LZ_KINDS_NUMBER, LZ_KINDS_ANY, 0,
                                  LZ_KIND_STRING | LZ_KIND_ARRAY};
    for (size_t i = 0; i < sizeof kind_sets / sizeof kind_sets[0]; i++) {
        types[type_count++] = lz_type_of_kinds(kind_sets[i]);
    }
    LzType literal_int = lz_type_of_kinds(0);
    literal_int.literal_count = 1;
    literal_int.literals[0].kind = LZ_VAL_INT;
    literal_int.literals[0].as.integer = 3;
    types[type_count++] = literal_int;
    LzType literal_bool = lz_type_of_kinds(0);
    literal_bool.literal_count = 1;
    literal_bool.literals[0].kind = LZ_VAL_BOOL;
    literal_bool.literals[0].as.boolean = true;
    types[type_count++] = literal_bool;
    LzType literal_text = lz_type_of_kinds(0);
    literal_text.literal_count = 1;
    literal_text.literals[0].kind = LZ_VAL_OBJECT;
    literal_text.literals[0].as.string = text;
    types[type_count++] = literal_text;
    LzType mixed = lz_type_of_kinds(LZ_KIND_FLOAT);
    mixed.literal_count = 1;
    mixed.literals[0].kind = LZ_VAL_INT;
    mixed.literals[0].as.integer = 3;
    types[type_count++] = mixed;

    LzValue values[] = {lz_value_nil(),      lz_value_bool(true),   lz_value_bool(false),
                        lz_value_int(3),     lz_value_int(4),       lz_value_float(1.5),
                        lz_value_object(&text->base), lz_value_object(&other->base)};
    for (size_t t = 0; t < type_count; t++) {
        for (size_t v = 0; v < sizeof values / sizeof values[0]; v++) {
            LzChunk chunk;
            lz_chunk_init(&chunk);
            uint16_t type_index = 0;
            LZ_CHECK(lz_chunk_add_type(&chunk, &types[t], &type_index));
            emit_u16(&chunk, LZ_OP_CONSTANT, constant_value(&chunk, values[v]), 1);
            emit_u16(&chunk, LZ_OP_CHECK_TYPE, type_index, 1);
            emit(&chunk, LZ_OP_POP, 1);
            emit(&chunk, LZ_OP_RETURN, 1);
            LzStatus status = lz_vm_run(&vm, &chunk);
            LZ_CHECK((status == LZ_OK) == lz_type_matches(&types[t], &values[v]));
            if (status != LZ_OK) {
                LZ_CHECK(status == LZ_ERR_TYPE);
                LZ_CHECK(strstr(vm.error, "type error: expected") != NULL);
                vm.stack_count = 0;
            }
            lz_chunk_free(&vm.heap, &chunk);
        }
    }
    LzChunk none;
    lz_chunk_init(&none);
    teardown(&vm, &none, &out, &err);
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
    test_fast_path_matches_general();
    test_check_type_shortcut_agrees();
    return LZ_TEST_RESULT();
}
