#include <math.h>
#include <stdint.h>
#include <string.h>

#include "lizard/compare.h"
#include "test_util.h"

static LzVM vm;

static LzValue integer(int64_t value) {
    return lz_value_int(value);
}

static LzValue number(double value) {
    return lz_value_float(value);
}

static void check_equal(LzValue a, LzValue b, bool expected) {
    LZ_CHECK(lz_value_equal(&a, &b) == expected);
    LZ_CHECK(lz_value_equal(&b, &a) == expected);
}

static void check_order(LzValue a, LzValue b, LzOrder expected) {
    LzOrder forward = lz_number_order(&a, &b);
    LZ_CHECK(forward == expected);

    LzOrder mirrored = expected;
    if (expected == LZ_ORDER_LESS) {
        mirrored = LZ_ORDER_GREATER;
    } else if (expected == LZ_ORDER_GREATER) {
        mirrored = LZ_ORDER_LESS;
    }
    LZ_CHECK(lz_number_order(&b, &a) == mirrored);
}

static void test_truthiness(void) {
    LzValue nil = lz_value_nil();
    LzValue no = lz_value_bool(false);
    LzValue yes = lz_value_bool(true);
    LzValue zero = integer(0);
    LzValue zero_float = number(0.0);

    LZ_CHECK(!lz_value_is_truthy(&nil));
    LZ_CHECK(!lz_value_is_truthy(&no));
    LZ_CHECK(lz_value_is_truthy(&yes));
    LZ_CHECK(lz_value_is_truthy(&zero));
    LZ_CHECK(lz_value_is_truthy(&zero_float));

    LzString *empty = lz_string_new(&vm.heap, "", 0);
    LzValue empty_value = lz_value_object(&empty->base);
    LZ_CHECK(lz_value_is_truthy(&empty_value));
    lz_object_release(&vm.heap, &empty->base);
}

static void test_equality(void) {
    check_equal(lz_value_nil(), lz_value_nil(), true);
    check_equal(lz_value_bool(true), lz_value_bool(true), true);
    check_equal(lz_value_bool(true), lz_value_bool(false), false);
    check_equal(integer(3), integer(3), true);
    check_equal(integer(3), integer(4), false);
    check_equal(number(1.5), number(1.5), true);

    check_equal(integer(1), number(1.0), true);
    check_equal(integer(0), number(-0.0), true);
    check_equal(integer(1), number(1.5), false);

    check_equal(lz_value_nil(), lz_value_bool(false), false);
    check_equal(integer(1), lz_value_bool(true), false);
    check_equal(integer(0), lz_value_nil(), false);

    check_equal(number(NAN), number(NAN), false);
    check_equal(integer(0), number(NAN), false);
}

static void test_exact_mixed_comparison(void) {
    check_equal(integer(9007199254740993), number(9007199254740992.0), false);
    check_order(integer(9007199254740993), number(9007199254740992.0), LZ_ORDER_GREATER);
    check_equal(integer(9007199254740992), number(9007199254740992.0), true);

    check_order(integer(INT64_MAX), number(9223372036854775808.0), LZ_ORDER_LESS);
    check_order(integer(INT64_MIN), number(-9223372036854775808.0), LZ_ORDER_EQUAL);
    check_order(integer(INT64_MIN), number(-1e300), LZ_ORDER_GREATER);
    check_order(integer(INT64_MAX), number(1e300), LZ_ORDER_LESS);
    check_order(integer(1), number(1.5), LZ_ORDER_LESS);
    check_order(integer(-1), number(-1.5), LZ_ORDER_GREATER);
    check_order(integer(2), number(INFINITY), LZ_ORDER_LESS);
    check_order(integer(2), number(NAN), LZ_ORDER_UNORDERED);
    check_order(number(NAN), number(1.0), LZ_ORDER_UNORDERED);
    check_order(number(1.0), number(2.0), LZ_ORDER_LESS);
}

static void test_string_equality(void) {
    LzString *first = lz_string_new(&vm.heap, "abc", 3);
    LzString *second = lz_string_new(&vm.heap, "abc", 3);
    LzString *other = lz_string_new(&vm.heap, "abd", 3);
    LzString *shorter = lz_string_new(&vm.heap, "ab", 2);

    LzValue a = lz_value_object(&first->base);
    LzValue b = lz_value_object(&second->base);
    LzValue c = lz_value_object(&other->base);
    LzValue d = lz_value_object(&shorter->base);

    LZ_CHECK(first != second);
    check_equal(a, a, true);
    check_equal(a, b, true);
    check_equal(a, c, false);
    check_equal(a, d, false);
    check_equal(a, integer(3), false);
    check_equal(a, lz_value_nil(), false);

    lz_object_release(&vm.heap, &first->base);
    lz_object_release(&vm.heap, &second->base);
    lz_object_release(&vm.heap, &other->base);
    lz_object_release(&vm.heap, &shorter->base);
}

static void check_compare(LzOpCode op, LzValue a, LzValue b, bool expected) {
    LzValue result = lz_value_nil();
    LZ_CHECK(lz_compare_order(&vm, op, &a, &b, &result) == LZ_OK);
    LZ_CHECK(result.type == LZ_VAL_BOOL);
    LZ_CHECK(result.as.boolean == expected);
}

static void test_ordering_operators(void) {
    check_compare(LZ_OP_LESS, integer(1), integer(2), true);
    check_compare(LZ_OP_LESS, integer(2), integer(2), false);
    check_compare(LZ_OP_GREATER, integer(3), integer(2), true);
    check_compare(LZ_OP_GREATER, number(1.5), integer(1), true);
    check_compare(LZ_OP_LESS, integer(1), number(1.5), true);
    check_compare(LZ_OP_LESS, number(NAN), number(1.0), false);
    check_compare(LZ_OP_GREATER, number(NAN), number(1.0), false);
    check_compare(LZ_OP_GREATER, integer(1), number(NAN), false);
    check_compare(LZ_OP_LESS_EQUAL, integer(2), integer(2), true);
    check_compare(LZ_OP_LESS_EQUAL, integer(3), integer(2), false);
    check_compare(LZ_OP_LESS_EQUAL, integer(1), number(1.0), true);
    check_compare(LZ_OP_LESS_EQUAL, number(NAN), number(NAN), false);
    check_compare(LZ_OP_LESS_EQUAL, number(1.0), number(NAN), false);
    check_compare(LZ_OP_GREATER_EQUAL, integer(2), integer(2), true);
    check_compare(LZ_OP_GREATER_EQUAL, integer(1), integer(2), false);
    check_compare(LZ_OP_GREATER_EQUAL, number(2.5), integer(2), true);
    check_compare(LZ_OP_GREATER_EQUAL, number(NAN), number(NAN), false);

    LzValue result = lz_value_nil();
    LzValue text = lz_value_bool(true);
    LzValue one = integer(1);
    LZ_CHECK(lz_compare_order(&vm, LZ_OP_LESS, &one, &text, &result) == LZ_ERR_RUNTIME);
    LZ_CHECK(strstr(vm.error, "operands must be numbers") != NULL);
    LZ_CHECK(lz_compare_order(&vm, LZ_OP_GREATER, &text, &one, &result) == LZ_ERR_RUNTIME);
}

int main(void) {
    LzBuffer out = {0};
    lz_vm_init(&vm, lz_writer_from_buffer(&out), lz_writer_from_buffer(&out));

    test_truthiness();
    test_equality();
    test_exact_mixed_comparison();
    test_string_equality();
    test_ordering_operators();

    LZ_CHECK(vm.heap.live_objects == 0);
    lz_vm_free(&vm);
    lz_buffer_free(&out);
    return LZ_TEST_RESULT();
}
