#include <string.h>

#include "convert/convert.h"
#include "io/io.h"
#include "lizard/builtin.h"

static const LzBuiltin builtins[] = {
    {"print", lz_builtin_print, 0, LZ_VARIADIC, LZ_KIND_NIL},
    {"println", lz_builtin_println, 0, LZ_VARIADIC, LZ_KIND_NIL},
    {"int", lz_builtin_int, 1, 1, LZ_KIND_INT},
    {"float", lz_builtin_float, 1, 1, LZ_KIND_FLOAT},
    {"string", lz_builtin_string, 1, 1, LZ_KIND_STRING},
    {"bool", lz_builtin_bool, 1, 1, LZ_KIND_BOOL},
};

#define BUILTIN_COUNT (sizeof builtins / sizeof builtins[0])

const LzBuiltin *lz_builtin_lookup(const char *name) {
    for (size_t i = 0; i < BUILTIN_COUNT; i++) {
        if (strcmp(builtins[i].name, name) == 0) {
            return &builtins[i];
        }
    }
    return NULL;
}

size_t lz_builtin_count(void) {
    return BUILTIN_COUNT;
}

const LzBuiltin *lz_builtin_at(size_t index) {
    return index < BUILTIN_COUNT ? &builtins[index] : NULL;
}

LzStatus lz_builtin_install(LzVM *vm) {
    for (size_t i = 0; i < BUILTIN_COUNT; i++) {
        const LzBuiltin *builtin = &builtins[i];

        LzString *name = lz_vm_intern(vm, builtin->name, strlen(builtin->name));
        LzNative *native = name == NULL ? NULL
                                        : lz_native_new(&vm->heap, name, builtin->fn,
                                                        builtin->min_args, builtin->max_args);
        if (native == NULL) {
            return lz_vm_out_of_memory(vm);
        }

        native->result_kinds = builtin->result_kinds;
        bool defined = lz_vm_define_builtin(vm, name, lz_value_object(&native->base));
        lz_object_release(&vm->heap, &native->base);
        if (!defined) {
            return lz_vm_out_of_memory(vm);
        }
    }
    return LZ_OK;
}
