#include <math.h>
#include <string.h>

#include "lizard/builtin.h"
#include "lizard/number.h"
#include "native_helpers.h"
#include "test_util.h"

static LzVM vm;

static LzValue text(const char *chars) {
    return lz_value_object(&lz_string_new(&vm.heap, chars, strlen(chars))->base);
}

static LzStatus call(const char *name, LzValue argument, LzValue *result) {
    const LzNative *native = find_native(&vm, name);
    LZ_CHECK(native != NULL);
    *result = lz_value_nil();
    return lz_native_call(&vm, native, 1, &argument, result);
}

static void check_int(const char *name, LzValue argument, int64_t expected) {
    LzValue result;
    LZ_CHECK(call(name, argument, &result) == LZ_OK);
    LZ_CHECK(result.type == LZ_VAL_INT && result.as.integer == expected);
}

static void check_float(LzValue argument, double expected) {
    LzValue result;
    LZ_CHECK(call("float", argument, &result) == LZ_OK);
    LZ_CHECK(result.type == LZ_VAL_FLOAT && result.as.number == expected);
}

static void check_failure(const char *name, LzValue argument, const char *message) {
    LzValue result;
    LZ_CHECK(call(name, argument, &result) == LZ_ERR_RUNTIME);
    LZ_CHECK(strstr(vm.error, message) != NULL);
}

static void check_string(LzValue argument, const char *expected) {
    LzValue result;
    LZ_CHECK(call("string", argument, &result) == LZ_OK);
    LZ_CHECK(result.type == LZ_VAL_OBJECT && result.as.object->type == LZ_OBJ_STRING);
    if (result.type == LZ_VAL_OBJECT) {
        const LzString *string = (const LzString *)result.as.object;
        LZ_CHECK(string->length == strlen(expected) && strcmp(string->data, expected) == 0);
    }
    lz_value_release(&vm.heap, &result);
}

static void test_int(void) {
    check_int("int", lz_value_int(7), 7);
    check_int("int", lz_value_float(3.99), 3);
    check_int("int", lz_value_float(-3.99), -3);
    check_int("int", lz_value_float(-0.5), 0);
    check_int("int", text("12"), 12);
    check_int("int", text("-7"), -7);
    check_int("int", text("+3"), 3);
    check_int("int", text("007"), 7);
    check_int("int", text("9223372036854775807"), INT64_MAX);
    check_int("int", text("-9223372036854775808"), INT64_MIN);

    check_failure("int", text("abc"), "invalid integer \"abc\"");
    check_failure("int", text(""), "invalid integer");
    check_failure("int", text(" 1"), "invalid integer");
    check_failure("int", text("1_0"), "invalid integer");
    check_failure("int", text("1.5"), "invalid integer");
    check_failure("int", text("0x10"), "invalid integer");
    check_failure("int", text("-"), "invalid integer");
    check_failure("int", text("9223372036854775808"), "out of range");
    check_failure("int", text("-9223372036854775809"), "out of range");
    check_failure("int", lz_value_float(NAN), "cannot convert nan");
    check_failure("int", lz_value_float(INFINITY), "cannot convert infinity");
    check_failure("int", lz_value_float(1e30), "out of range");
    check_failure("int", lz_value_float(-9223372036854777856.0), "out of range");
    check_failure("int", lz_value_nil(), "cannot convert nil");
    check_failure("int", lz_value_bool(true), "cannot convert bool");
    check_failure("int", text("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"), "...") ;
}

static void test_float(void) {
    check_float(lz_value_int(3), 3.0);
    check_float(lz_value_float(2.5), 2.5);
    check_float(text("1.5"), 1.5);
    check_float(text("1.5e3"), 1500.0);
    check_float(text("-2"), -2.0);
    check_float(text("+0.25"), 0.25);
    check_float(text("1E-2"), 0.01);

    check_failure("float", text(""), "invalid float");
    check_failure("float", text(".5"), "invalid float");
    check_failure("float", text("5."), "invalid float");
    check_failure("float", text("1e"), "invalid float");
    check_failure("float", text("nan"), "invalid float");
    check_failure("float", text("inf"), "invalid float");
    check_failure("float", text("0x10"), "invalid float");
    check_failure("float", text(" 1.5"), "invalid float");
    check_failure("float", text("1_0.5"), "invalid float");
    check_failure("float", text("1e999"), "out of range");
    check_failure("float", lz_value_nil(), "cannot convert nil");
}

static void test_string(void) {
    check_string(lz_value_int(42), "42");
    check_string(lz_value_int(-7), "-7");
    check_string(lz_value_float(1.5), "1.5");
    check_string(lz_value_float(3.0), "3.0");
    check_string(lz_value_float(NAN), "nan");
    check_string(lz_value_bool(true), "true");
    check_string(lz_value_nil(), "nil");
    check_string(text("same"), "same");

    LzValue original = text("kept");
    LzValue result;
    LZ_CHECK(call("string", original, &result) == LZ_OK);
    LZ_CHECK(result.as.object == original.as.object);
    LZ_CHECK(result.as.object->refcount == 2);
    lz_value_release(&vm.heap, &result);

    const LzNative *println = find_native(&vm, "println");
    LzValue function = lz_value_object((LzObject *)println);
    check_string(function, "<native println>");
}

static void test_bool(void) {
    LzValue result;
    LZ_CHECK(call("bool", lz_value_nil(), &result) == LZ_OK && !result.as.boolean);
    LZ_CHECK(call("bool", lz_value_bool(false), &result) == LZ_OK && !result.as.boolean);
    LZ_CHECK(call("bool", lz_value_int(0), &result) == LZ_OK && result.as.boolean);
    LZ_CHECK(call("bool", text(""), &result) == LZ_OK && result.as.boolean);
    LZ_CHECK(call("bool", lz_value_float(0.0), &result) == LZ_OK && result.as.boolean);
}

static void test_result_kinds(void) {
    LZ_CHECK(find_native(&vm, "int")->result_kinds == LZ_KIND_INT);
    LZ_CHECK(find_native(&vm, "float")->result_kinds == LZ_KIND_FLOAT);
    LZ_CHECK(find_native(&vm, "string")->result_kinds == LZ_KIND_STRING);
    LZ_CHECK(find_native(&vm, "bool")->result_kinds == LZ_KIND_BOOL);
    LZ_CHECK(find_native(&vm, "println")->result_kinds == LZ_KIND_NIL);

    LzString *name = lz_vm_intern(&vm, "host", 4);
    LzNative *host = lz_native_new(&vm.heap, name, NULL, 0, 0);
    LZ_CHECK(host != NULL && host->result_kinds == LZ_KINDS_ANY);
    lz_object_release(&vm.heap, &host->base);
}

static void test_arity(void) {
    const LzNative *native = find_native(&vm, "int");
    LzValue result;
    LZ_CHECK(lz_native_call(&vm, native, 0, NULL, &result) == LZ_ERR_ARITY);
    LzValue two[] = {lz_value_int(1), lz_value_int(2)};
    LZ_CHECK(lz_native_call(&vm, native, 2, two, &result) == LZ_ERR_ARITY);
}

static void test_number_parsers(void) {
    int64_t integer = 0;
    LZ_CHECK(lz_parse_integer("0", 1, &integer) == LZ_PARSE_OK && integer == 0);
    LZ_CHECK(lz_parse_integer("-0", 2, &integer) == LZ_PARSE_OK && integer == 0);
    LZ_CHECK(lz_parse_integer("", 0, &integer) == LZ_PARSE_SYNTAX);
    LZ_CHECK(lz_parse_integer("+", 1, &integer) == LZ_PARSE_SYNTAX);
    LZ_CHECK(lz_parse_integer("99999999999999999999", 20, &integer) == LZ_PARSE_RANGE);
    LZ_CHECK(lz_parse_integer("1 ", 2, &integer) == LZ_PARSE_SYNTAX);
    LZ_CHECK(lz_parse_integer("12", 1, &integer) == LZ_PARSE_OK && integer == 1);

    double number = 0;
    LZ_CHECK(lz_parse_float("0.1", 3, &number) == LZ_PARSE_OK && number == 0.1);
    LZ_CHECK(lz_parse_float("1e5", 3, &number) == LZ_PARSE_OK && number == 100000.0);
    LZ_CHECK(lz_parse_float("-1.5E+2", 7, &number) == LZ_PARSE_OK && number == -150.0);
    LZ_CHECK(lz_parse_float("1e-999", 6, &number) == LZ_PARSE_OK && number == 0.0);
    LZ_CHECK(lz_parse_float("1e999", 5, &number) == LZ_PARSE_RANGE);
    LZ_CHECK(lz_parse_float("1.", 2, &number) == LZ_PARSE_SYNTAX);
    LZ_CHECK(lz_parse_float("--1", 3, &number) == LZ_PARSE_SYNTAX);
    LZ_CHECK(lz_parse_float("1.5", 2, &number) == LZ_PARSE_SYNTAX);
}

int main(void) {
    LzBuffer out = {0};
    lz_vm_init(&vm, lz_writer_from_buffer(&out), lz_writer_from_buffer(&out));
    LZ_CHECK(lz_builtin_install(&vm) == LZ_OK);

    test_int();
    test_float();
    test_string();
    test_bool();
    test_result_kinds();
    test_arity();
    test_number_parsers();

    lz_vm_free(&vm);
    lz_buffer_free(&out);
    return LZ_TEST_RESULT();
}
