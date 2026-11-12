#include <string.h>

#include "array/array.h"
#include "array/higher.h"
#include "convert/convert.h"
#include "io/io.h"
#include "lizard/builtin.h"
#include "method/method.h"

static const LzBuiltin builtins[] = {
    {"print", lz_builtin_print, 0, LZ_VARIADIC, LZ_KIND_NIL},
    {"println", lz_builtin_println, 0, LZ_VARIADIC, LZ_KIND_NIL},
    {"int", lz_builtin_int, 1, 1, LZ_KIND_INT},
    {"float", lz_builtin_float, 1, 1, LZ_KIND_FLOAT},
    {"string", lz_builtin_string, 1, 1, LZ_KIND_STRING},
    {"bool", lz_builtin_bool, 1, 1, LZ_KIND_BOOL},
    {"identical", lz_builtin_identical, 2, 2, LZ_KIND_BOOL},
    {"link", lz_builtin_link, 1, 1, LZ_KIND_INSTANCE},
    {"typeof", lz_builtin_typeof, 1, 1, LZ_KIND_STRING},
    {"len", lz_builtin_len, 1, 1, LZ_KIND_INT},
    {"push", lz_builtin_push, 2, 2, LZ_KIND_NIL},
    {"pop", lz_builtin_pop, 1, 1, LZ_KINDS_ANY},
    {"insert", lz_builtin_insert, 3, 3, LZ_KIND_NIL},
    {"remove", lz_builtin_remove, 2, 2, LZ_KINDS_ANY},
    {"clear", lz_builtin_clear, 1, 1, LZ_KIND_NIL},
    {"copy", lz_builtin_copy, 1, 1, LZ_KIND_ARRAY},
    {"contains", lz_builtin_contains, 2, 2, LZ_KIND_BOOL},
    {"index_of", lz_builtin_index_of, 2, 2, LZ_KIND_INT},
    {"reverse", lz_builtin_reverse, 1, 1, LZ_KIND_NIL},
    {"sort", lz_builtin_sort, 1, 1, LZ_KIND_NIL},
    {"join", lz_builtin_join, 2, 2, LZ_KIND_STRING},
    {"range", lz_builtin_range, 1, 2, LZ_KIND_ARRAY},
    {"sum", lz_builtin_sum, 1, 1, LZ_KINDS_NUMBER},
    {"min", lz_builtin_min, 1, 1, LZ_KINDS_NUMBER},
    {"max", lz_builtin_max, 1, 1, LZ_KINDS_NUMBER},
    {"map", lz_builtin_map, 2, 2, LZ_KIND_ARRAY},
    {"filter", lz_builtin_filter, 2, 2, LZ_KIND_ARRAY},
    {"reduce", lz_builtin_reduce, 3, 3, LZ_KINDS_ANY},
    {"each", lz_builtin_each, 2, 2, LZ_KIND_NIL},
    {"find", lz_builtin_find, 2, 2, LZ_KINDS_ANY},
    {"any", lz_builtin_any, 2, 2, LZ_KIND_BOOL},
    {"all", lz_builtin_all, 2, 2, LZ_KIND_BOOL},
    /* The kinds of error: each makes an error value from a message. */
    {"Error", lz_error_type_native, 1, 1, LZ_KIND_ERROR},
    {"RuntimeError", lz_error_type_native, 1, 1, LZ_KIND_ERROR},
    {"TypeError", lz_error_type_native, 1, 1, LZ_KIND_ERROR},
    {"ValueError", lz_error_type_native, 1, 1, LZ_KIND_ERROR},
    {"RangeError", lz_error_type_native, 1, 1, LZ_KIND_ERROR},
    {"ArithmeticError", lz_error_type_native, 1, 1, LZ_KIND_ERROR},
    {"ArityError", lz_error_type_native, 1, 1, LZ_KIND_ERROR},
    {"IOError", lz_error_type_native, 1, 1, LZ_KIND_ERROR},
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
        /* The few builtins that are about the cell itself or only print it. */
        native->takes_links = (builtin->fn == lz_builtin_print || builtin->fn == lz_builtin_println ||
                               builtin->fn == lz_builtin_string || builtin->fn == lz_builtin_identical ||
                               builtin->fn == lz_builtin_link || builtin->fn == lz_builtin_typeof)
                                  ? LZ_LINKS_ANY
                              : (builtin->fn == lz_builtin_push || builtin->fn == lz_builtin_insert)
                                  ? LZ_LINKS_ITEMS
                                  : LZ_LINKS_NONE;
        uint8_t error_kind;
        if (builtin->fn == lz_error_type_native &&
            lz_vm_error_kind_from_name(vm, NULL, builtin->name, strlen(builtin->name), &error_kind)) {
            native->data = (const void *)(uintptr_t)error_kind; /* the kind this function makes */
        }
        bool defined = lz_vm_define_builtin(vm, name, lz_value_object(&native->base));
        lz_object_release(&vm->heap, &native->base);
        if (!defined) {
            return lz_vm_out_of_memory(vm);
        }
    }
    return lz_method_install(vm);
}
