#include <stdint.h>
#include <string.h>

#include "lizard/arith.h"
#include "test_util.h"

static LzVM vm;

static LzStatus run_binary(LzOpCode op, LzValue left, LzValue right, LzValue *result) {
    return lz_arith_binary(&vm, op, &left, &right, result);
}

static void check_int(LzOpCode op, int64_t a, int64_t b, int64_t expected) {
    LzValue result = lz_value_nil();
    LZ_CHECK(run_binary(op, lz_value_int(a), lz_value_int(b), &result) == LZ_OK);
    LZ_CHECK(result.type == LZ_VAL_INT);
    LZ_CHECK(result.as.integer == expected);
}

static void check_float(LzOpCode op, LzValue a, LzValue b, double expected) {
    LzValue result = lz_value_nil();
    LZ_CHECK(run_binary(op, a, b, &result) == LZ_OK);
    LZ_CHECK(result.type == LZ_VAL_FLOAT);
    LZ_CHECK(result.as.number == expected);
}

static void check_error(LzOpCode op, LzValue a, LzValue b, const char *message) {
    LzValue result = lz_value_nil();
    LZ_CHECK(run_binary(op, a, b, &result) == LZ_ERR_RUNTIME);
    LZ_CHECK(strstr(vm.error, message) != NULL);
}

static void test_integers(void) {
    check_int(LZ_OP_ADD, 2, 3, 5);
    check_int(LZ_OP_SUBTRACT, 2, 5, -3);
    check_int(LZ_OP_MULTIPLY, -4, 6, -24);
    check_int(LZ_OP_ADD, INT64_MAX, 0, INT64_MAX);
    check_int(LZ_OP_SUBTRACT, INT64_MIN, 0, INT64_MIN);
    check_int(LZ_OP_MULTIPLY, INT64_MAX, 1, INT64_MAX);
    check_int(LZ_OP_MULTIPLY, INT64_MIN, 1, INT64_MIN);
    check_int(LZ_OP_MULTIPLY, 0, INT64_MIN, 0);
}

static void test_floats_and_mixing(void) {
    check_float(LZ_OP_ADD, lz_value_float(1.5), lz_value_float(2.0), 3.5);
    check_float(LZ_OP_ADD, lz_value_int(1), lz_value_float(0.5), 1.5);
    check_float(LZ_OP_MULTIPLY, lz_value_float(2.5), lz_value_int(2), 5.0);
    check_float(LZ_OP_SUBTRACT, lz_value_int(1), lz_value_float(0.25), 0.75);
}

static void test_division(void) {
    check_float(LZ_OP_DIVIDE, lz_value_int(7), lz_value_int(2), 3.5);
    check_float(LZ_OP_DIVIDE, lz_value_int(6), lz_value_int(3), 2.0);
    check_float(LZ_OP_DIVIDE, lz_value_float(1.0), lz_value_int(4), 0.25);
    check_error(LZ_OP_DIVIDE, lz_value_int(1), lz_value_int(0), "division by zero");
    check_error(LZ_OP_DIVIDE, lz_value_float(1.0), lz_value_float(0.0), "division by zero");
    check_error(LZ_OP_DIVIDE, lz_value_int(0), lz_value_int(0), "division by zero");
}

static void test_overflow(void) {
    check_error(LZ_OP_ADD, lz_value_int(INT64_MAX), lz_value_int(1), "integer overflow");
    check_error(LZ_OP_ADD, lz_value_int(INT64_MIN), lz_value_int(-1), "integer overflow");
    check_error(LZ_OP_SUBTRACT, lz_value_int(INT64_MIN), lz_value_int(1), "integer overflow");
    check_error(LZ_OP_SUBTRACT, lz_value_int(INT64_MAX), lz_value_int(-1), "integer overflow");
    check_error(LZ_OP_MULTIPLY, lz_value_int(INT64_MAX), lz_value_int(2), "integer overflow");
    check_error(LZ_OP_MULTIPLY, lz_value_int(INT64_MIN), lz_value_int(-1), "integer overflow");
    check_error(LZ_OP_MULTIPLY, lz_value_int(-1), lz_value_int(INT64_MIN), "integer overflow");
    check_error(LZ_OP_MULTIPLY, lz_value_int(INT64_MIN / 2), lz_value_int(3), "integer overflow");
    check_error(LZ_OP_MULTIPLY, lz_value_int(3037000500), lz_value_int(3037000500),
                "integer overflow");
}

static void test_type_errors(void) {
    check_error(LZ_OP_ADD, lz_value_bool(true), lz_value_int(1), "two numbers or two strings");
    check_error(LZ_OP_SUBTRACT, lz_value_int(1), lz_value_nil(), "operands must be numbers");
    check_error(LZ_OP_DIVIDE, lz_value_nil(), lz_value_nil(), "operands must be numbers");
}

static void test_floor_division_and_modulo(void) {
    check_int(LZ_OP_FLOOR_DIVIDE, 7, 2, 3);
    check_int(LZ_OP_FLOOR_DIVIDE, -7, 2, -4);
    check_int(LZ_OP_FLOOR_DIVIDE, 7, -2, -4);
    check_int(LZ_OP_FLOOR_DIVIDE, -7, -2, 3);
    check_int(LZ_OP_FLOOR_DIVIDE, 6, 3, 2);
    check_int(LZ_OP_FLOOR_DIVIDE, 0, 5, 0);
    check_int(LZ_OP_FLOOR_DIVIDE, INT64_MIN, 1, INT64_MIN);
    check_int(LZ_OP_FLOOR_DIVIDE, 5, -1, -5);
    check_error(LZ_OP_FLOOR_DIVIDE, lz_value_int(INT64_MIN), lz_value_int(-1), "integer overflow");
    check_error(LZ_OP_FLOOR_DIVIDE, lz_value_int(1), lz_value_int(0), "division by zero");

    check_int(LZ_OP_MODULO, 7, 3, 1);
    check_int(LZ_OP_MODULO, -7, 3, 2);
    check_int(LZ_OP_MODULO, 7, -3, -2);
    check_int(LZ_OP_MODULO, -7, -3, -1);
    check_int(LZ_OP_MODULO, 6, 3, 0);
    check_int(LZ_OP_MODULO, INT64_MIN, -1, 0);
    check_int(LZ_OP_MODULO, INT64_MIN, 3, 1);
    check_error(LZ_OP_MODULO, lz_value_int(1), lz_value_int(0), "division by zero");

    check_float(LZ_OP_FLOOR_DIVIDE, lz_value_float(7.5), lz_value_int(2), 3.0);
    check_float(LZ_OP_FLOOR_DIVIDE, lz_value_float(-7.5), lz_value_int(2), -4.0);
    check_float(LZ_OP_FLOOR_DIVIDE, lz_value_int(7), lz_value_float(2.0), 3.0);
    check_float(LZ_OP_FLOOR_DIVIDE, lz_value_float(1.0), lz_value_float(0.1), 9.0);
    check_float(LZ_OP_MODULO, lz_value_float(7.5), lz_value_int(2), 1.5);
    check_float(LZ_OP_MODULO, lz_value_float(-7.5), lz_value_int(2), 0.5);
    check_float(LZ_OP_MODULO, lz_value_float(5.5), lz_value_int(-2), -0.5);
    check_error(LZ_OP_FLOOR_DIVIDE, lz_value_float(1.0), lz_value_float(0.0), "division by zero");
    check_error(LZ_OP_MODULO, lz_value_int(1), lz_value_float(0.0), "division by zero");
    check_error(LZ_OP_MODULO, lz_value_nil(), lz_value_int(2), "operands must be numbers");
}

static void check_text(const LzValue *value, const char *expected) {
    LZ_CHECK(value->type == LZ_VAL_OBJECT && value->as.object->type == LZ_OBJ_STRING);
    if (value->type == LZ_VAL_OBJECT && value->as.object->type == LZ_OBJ_STRING) {
        const LzString *text = (const LzString *)value->as.object;
        LZ_CHECK(text->length == strlen(expected));
        LZ_CHECK(strcmp(text->data, expected) == 0);
        LZ_CHECK(text->hash == lz_hash_bytes(expected, strlen(expected)));
    }
}

static void test_string_concat(void) {
    LzString *left = lz_string_new(&vm.heap, "ab", 2);
    LzString *right = lz_string_new(&vm.heap, "cd", 2);
    LzString *empty = lz_string_new(&vm.heap, "", 0);
    LzValue a = lz_value_object(&left->base);
    LzValue b = lz_value_object(&right->base);
    LzValue e = lz_value_object(&empty->base);

    LzValue joined = lz_value_nil();
    LZ_CHECK(run_binary(LZ_OP_ADD, a, b, &joined) == LZ_OK);
    check_text(&joined, "abcd");
    LZ_CHECK(joined.as.object->refcount == 1);
    LZ_CHECK(left->length == 2 && right->length == 2);
    lz_value_release(&vm.heap, &joined);

    LZ_CHECK(run_binary(LZ_OP_ADD, e, b, &joined) == LZ_OK);
    check_text(&joined, "cd");
    LZ_CHECK(joined.as.object != &right->base);
    lz_value_release(&vm.heap, &joined);

    LZ_CHECK(run_binary(LZ_OP_ADD, e, e, &joined) == LZ_OK);
    check_text(&joined, "");
    lz_value_release(&vm.heap, &joined);

    check_error(LZ_OP_ADD, a, lz_value_int(1), "two numbers or two strings");
    check_error(LZ_OP_ADD, lz_value_int(1), a, "two numbers or two strings");
    check_error(LZ_OP_ADD, lz_value_nil(), lz_value_nil(), "two numbers or two strings");
    check_error(LZ_OP_SUBTRACT, a, b, "operands must be numbers");

    lz_object_release(&vm.heap, &left->base);
    lz_object_release(&vm.heap, &right->base);
    lz_object_release(&vm.heap, &empty->base);
    LZ_CHECK(vm.heap.live_objects == 0);
}

static void test_negate(void) {
    LzValue result = lz_value_nil();

    LzValue five = lz_value_int(5);
    LZ_CHECK(lz_arith_negate(&vm, &five, &result) == LZ_OK);
    LZ_CHECK(result.type == LZ_VAL_INT && result.as.integer == -5);

    LzValue half = lz_value_float(0.5);
    LZ_CHECK(lz_arith_negate(&vm, &half, &result) == LZ_OK);
    LZ_CHECK(result.type == LZ_VAL_FLOAT && result.as.number == -0.5);

    LzValue minimum = lz_value_int(INT64_MIN);
    LZ_CHECK(lz_arith_negate(&vm, &minimum, &result) == LZ_ERR_RUNTIME);
    LZ_CHECK(strstr(vm.error, "integer overflow") != NULL);

    LzValue flag = lz_value_bool(false);
    LZ_CHECK(lz_arith_negate(&vm, &flag, &result) == LZ_ERR_RUNTIME);
    LZ_CHECK(strstr(vm.error, "operand must be a number") != NULL);
}

int main(void) {
    LzBuffer out = {0};
    lz_vm_init(&vm, lz_writer_from_buffer(&out), lz_writer_from_buffer(&out));

    test_integers();
    test_floats_and_mixing();
    test_division();
    test_overflow();
    test_type_errors();
    test_floor_division_and_modulo();
    test_string_concat();
    test_negate();

    lz_vm_free(&vm);
    lz_buffer_free(&out);
    return LZ_TEST_RESULT();
}
