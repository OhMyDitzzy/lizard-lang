#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "lizard/builtin.h"
#include "native_helpers.h"
#include "test_util.h"

static void reset(LzBuffer *buffer) {
    buffer->length = 0;
    if (buffer->data != NULL) {
        buffer->data[0] = '\0';
    }
}

static bool output_is(const LzBuffer *buffer, const char *expected) {
    return buffer->data != NULL && strcmp(buffer->data, expected) == 0;
}

int main(void) {
    LzBuffer out = {0};
    LzBuffer err = {0};
    LzVM vm;
    lz_vm_init(&vm, lz_writer_from_buffer(&out), lz_writer_from_buffer(&err));
    LZ_CHECK(lz_builtin_install(&vm) == LZ_OK);

    const LzNative *print = find_native(&vm, "print");
    const LzNative *println = find_native(&vm, "println");
    LZ_CHECK(print != NULL);
    LZ_CHECK(println != NULL);
    if (print == NULL || println == NULL) {
        return 1;
    }

    LzString *greeting = lz_string_new(&vm.heap, "hello", 5);
    LzValue mixed[] = {lz_value_object(&greeting->base), lz_value_int(42), lz_value_bool(true),
                       lz_value_nil()};
    LzValue result = lz_value_bool(true);

    LZ_CHECK(lz_native_call(&vm, println, 4, mixed, &result) == LZ_OK);
    LZ_CHECK(output_is(&out, "hello 42 true nil\n"));
    LZ_CHECK(result.type == LZ_VAL_NIL);

    reset(&out);
    LZ_CHECK(lz_native_call(&vm, print, 2, mixed, &result) == LZ_OK);
    LZ_CHECK(output_is(&out, "hello 42"));

    reset(&out);
    LZ_CHECK(lz_native_call(&vm, println, 0, NULL, &result) == LZ_OK);
    LZ_CHECK(output_is(&out, "\n"));

    reset(&out);
    LzValue floats[] = {lz_value_float(0.1), lz_value_float(3.0), lz_value_float(-2.5)};
    LZ_CHECK(lz_native_call(&vm, print, 3, floats, &result) == LZ_OK);
    LZ_CHECK(output_is(&out, "0.1 3.0 -2.5"));

    /* The text of NaN and the infinities does not depend on the platform's printf. */
    reset(&out);
    LzValue special[] = {lz_value_float(NAN), lz_value_float(-NAN), lz_value_float(INFINITY),
                         lz_value_float(-INFINITY)};
    LZ_CHECK(lz_native_call(&vm, print, 4, special, &result) == LZ_OK);
    LZ_CHECK(output_is(&out, "nan nan inf -inf"));

    /* A long string costs one fuel unit per 64 bytes, charged before anything is written. */
    size_t big_length = 64 * 1000;
    char *big_text = malloc(big_length);
    LZ_CHECK(big_text != NULL);
    if (big_text != NULL) {
        memset(big_text, 'x', big_length);
        LzString *big = lz_string_new(&vm.heap, big_text, big_length);
        free(big_text);
        LZ_CHECK(big != NULL);
        if (big != NULL) {
            LzValue big_value = lz_value_object(&big->base);
            reset(&out);
            lz_vm_set_fuel(&vm, 999);
            LZ_CHECK(lz_native_call(&vm, print, 1, &big_value, &result) == LZ_ERR_FUEL);
            LZ_CHECK(out.length == 0);
            lz_vm_set_fuel(&vm, 1000);
            LZ_CHECK(lz_native_call(&vm, print, 1, &big_value, &result) == LZ_OK);
            LZ_CHECK(vm.fuel == 0);
            LZ_CHECK(out.length == big_length);
            lz_vm_set_fuel(&vm, LZ_FUEL_UNLIMITED);
            lz_object_release(&vm.heap, &big->base);
        }
    }

    lz_object_release(&vm.heap, &greeting->base);
    lz_vm_release_roots(&vm);
    LZ_CHECK(vm.heap.live_objects == 0);

    lz_vm_free(&vm);
    lz_buffer_free(&out);
    lz_buffer_free(&err);
    return LZ_TEST_RESULT();
}
