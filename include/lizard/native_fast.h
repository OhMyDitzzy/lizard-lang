#ifndef LIZARD_NATIVE_FAST_H
#define LIZARD_NATIVE_FAST_H

#include "lizard/array.h"
#include "lizard/link.h"
#include "lizard/native.h"
#include "lizard/vm.h"

/*
 * The short way to call a native from a CALL instruction. A native pushes no frame, so what
 * lz_vm_call adds for one is only a few checks. A native that is marked LZ_NATIVE_LEAF (or
 * LEAF_SCALARS, with arguments that fit) is called straight from the handler instead. Everything
 * that is not the plain success of such a call goes back to lz_native_call, which reports the
 * arity and link errors with the same message, so an error looks the same on both ways.
 */

/* The characters of UTF-8 text: the bytes that are not continuation bytes. */
static inline int64_t lz_utf8_length(const char *data, size_t length) {
    int64_t count = 0;
    for (size_t i = 0; i < length; i++) {
        if (((unsigned char)data[i] & 0xC0) != 0x80) {
            count++;
        }
    }
    return count;
}

/* Is this call one that lz_native_call_fast may make? */
static inline bool lz_native_is_leaf(const LzNative *native, int argc, const LzValue *argv) {
    if ((native->flags & LZ_NATIVE_LEAF) != 0) {
        return true;
    }
    if ((native->flags & LZ_NATIVE_LEAF_SCALARS) == 0) {
        return false;
    }
    for (int i = 0; i < argc; i++) {
        if (argv[i].type == LZ_VAL_OBJECT && argv[i].as.object->type != LZ_OBJ_STRING) {
            return false;
        }
    }
    return true;
}

/*
 * Same result, same error and same fuel as lz_native_call, for a native that lz_native_is_leaf
 * accepted. The caller sets vm->native_call_offset first, as it does for the long way.
 */
static inline LzStatus lz_native_call_fast(LzVM *vm, const LzNative *native, int argc, const LzValue *argv,
                                           LzValue *result) {
    if (argc < native->min_args || (native->max_args != LZ_VARIADIC && argc > native->max_args)) {
        return lz_native_call(vm, native, argc, argv, result);
    }
    if (native->takes_links != LZ_LINKS_ANY) {
        for (int i = 0; i < argc && (i == 0 || native->takes_links == LZ_LINKS_NONE); i++) {
            if (lz_value_is_link(&argv[i])) {
                return lz_native_call(vm, native, argc, argv, result);
            }
        }
    }
    if ((native->flags & LZ_NATIVE_LEN) != 0) {
        if (lz_value_is_array(&argv[0])) {
            *result = lz_value_int((int64_t)lz_value_as_array(&argv[0])->count);
            return LZ_OK;
        }
        if (argv[0].type == LZ_VAL_OBJECT && argv[0].as.object->type == LZ_OBJ_STRING) {
            const LzString *text = (const LzString *)argv[0].as.object;
            LzStatus charged = lz_vm_charge_fuel(vm, text->length / 64);
            if (charged != LZ_OK) {
                return charged;
            }
            *result = lz_value_int(lz_utf8_length(text->data, text->length));
            return LZ_OK;
        }
    }
    vm->current_native = native; /* read at entry by natives that share one function, see methods */
    return native->fn(vm, argc, argv, result);
}

#endif
