#include <string.h>

#include "lizard/builtin.h"
#include "native_helpers.h"
#include "test_util.h"

static LzStatus do_nothing(LzVM *vm, int argc, const LzValue *argv, LzValue *result) {
    (void)vm;
    (void)argc;
    (void)argv;
    *result = lz_value_nil();
    return LZ_OK;
}

int main(void) {
    LzBuffer out = {0};
    LzBuffer err = {0};
    LzVM vm;
    lz_vm_init(&vm, lz_writer_from_buffer(&out), lz_writer_from_buffer(&err));

    LZ_CHECK(lz_builtin_install(&vm) == LZ_OK);
    LZ_CHECK(vm.builtins.count == lz_builtin_count());

    const LzNative *print = find_native(&vm, "print");
    LZ_CHECK(print != NULL);
    if (print != NULL) {
        LZ_CHECK(strcmp(print->name->data, "print") == 0);
    }
    LZ_CHECK(find_native(&vm, "no_such_builtin") == NULL);

    LzString *print_name = lz_vm_intern(&vm, "print", 5);
    LZ_CHECK(print_name != NULL);
    LZ_CHECK(lz_vm_define_global(&vm, print_name, lz_value_int(7)));
    const LzValue *shadowed = lz_vm_lookup(&vm, print_name);
    LZ_CHECK(shadowed != NULL && shadowed->type == LZ_VAL_INT && shadowed->as.integer == 7);

    LzString *one_name = lz_vm_intern(&vm, "one", 3);
    LzNative *strict = lz_native_new(&vm.heap, one_name, do_nothing, 1, 1);
    LZ_CHECK(strict != NULL);
    LzValue result = lz_value_nil();
    LzValue arg = lz_value_int(1);
    LZ_CHECK(lz_native_call(&vm, strict, 0, NULL, &result) == LZ_ERR_ARITY);
    LZ_CHECK(strstr(vm.error, "one()") != NULL);
    LZ_CHECK(lz_native_call(&vm, strict, 2, (LzValue[]){arg, arg}, &result) == LZ_ERR_ARITY);
    LZ_CHECK(lz_native_call(&vm, strict, 1, &arg, &result) == LZ_OK);

    LzValue as_value = lz_value_object(&strict->base);
    LZ_CHECK(lz_value_write(&as_value, &vm.out));
    LZ_CHECK(out.data != NULL && strcmp(out.data, "<native one>") == 0);

    lz_object_release(&vm.heap, &strict->base);
    lz_vm_release_roots(&vm);
    LZ_CHECK(vm.heap.live_objects == 0);

    lz_vm_free(&vm);
    lz_buffer_free(&out);
    lz_buffer_free(&err);
    return LZ_TEST_RESULT();
}
