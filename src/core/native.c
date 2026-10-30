#include "lizard/native.h"

#include "lizard/vm.h"

LzNative *lz_native_new(LzHeap *heap, LzString *name, LzNativeFn fn, int min_args, int max_args) {
    LzNative *native = lz_heap_alloc(heap, LZ_OBJ_NATIVE, sizeof *native);
    if (native == NULL) {
        return NULL;
    }
    lz_object_retain(&name->base);
    native->name = name;
    native->fn = fn;
    native->min_args = min_args;
    native->max_args = max_args;
    native->result_kinds = LZ_KINDS_ANY;
    native->data = NULL;
    return native;
}

LzStatus lz_native_call(LzVM *vm, const LzNative *native, int argc, const LzValue *argv,
                        LzValue *result) {
    bool too_few = argc < native->min_args;
    bool too_many = native->max_args != LZ_VARIADIC && argc > native->max_args;
    if (too_few || too_many) {
        return lz_vm_set_error(vm, LZ_ERR_ARITY, "%s(): wrong number of arguments (%d given)",
                               native->name->data, argc);
    }
    vm->current_native = native; /* read at entry by natives that share one function, see methods */
    return native->fn(vm, argc, argv, result);
}
