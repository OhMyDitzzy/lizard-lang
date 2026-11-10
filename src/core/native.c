#include "lizard/native.h"

#include "lizard/link.h"
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
    native->takes_links = LZ_LINKS_NONE;
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
    if (native->takes_links != LZ_LINKS_ANY) {
        /*
         * A builtin works on the object, never on the cell that points at it. The ones that only
         * store their later arguments, like push and insert, take a cell there but not as the
         * subject.
         */
        for (int i = 0; i < argc && (i == 0 || native->takes_links == LZ_LINKS_NONE); i++) {
            if (lz_value_is_link(&argv[i])) {
                return lz_vm_link_error(vm);
            }
        }
    }
    vm->current_native = native; /* read at entry by natives that share one function, see methods */
    return native->fn(vm, argc, argv, result);
}
