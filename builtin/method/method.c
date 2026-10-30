#include "method/method.h"

#include <stdio.h>
#include <string.h>

#include "convert/convert.h"
#include "lizard/array.h"

/* ---- methods that need no file of their own ---- */

LzStatus lz_method_to_string(LzVM *vm, int argc, const LzValue *argv, LzValue *result) {
    return lz_builtin_string(vm, argc, argv, result);
}

LzStatus lz_method_is_empty(LzVM *vm, int argc, const LzValue *argv, LzValue *result) {
    (void)vm;
    (void)argc;
    if (lz_value_is_array(&argv[0])) {
        *result = lz_value_bool(lz_value_as_array(&argv[0])->count == 0);
    } else {
        *result = lz_value_bool(((const LzString *)argv[0].as.object)->length == 0);
    }
    return LZ_OK;
}

static LzStatus end_item(LzVM *vm, const char *name, const LzValue *argv, bool last, LzValue *result) {
    if (!lz_value_is_array(&argv[0])) {
        return lz_vm_set_error(vm, LZ_ERR_RUNTIME, "%s(): expected an array, got %s", name,
                               lz_kind_name(lz_kind_of(&argv[0])));
    }
    const LzArray *array = lz_value_as_array(&argv[0]);
    if (array->count == 0) {
        *result = lz_value_nil();
        return LZ_OK;
    }
    *result = array->items[last ? array->count - 1 : 0];
    lz_value_retain(result);
    return LZ_OK;
}

LzStatus lz_method_first(LzVM *vm, int argc, const LzValue *argv, LzValue *result) {
    (void)argc;
    return end_item(vm, "first", argv, false, result);
}

LzStatus lz_method_last(LzVM *vm, int argc, const LzValue *argv, LzValue *result) {
    (void)argc;
    return end_item(vm, "last", argv, true, result);
}

/* ---- the table ---- */

#define K_TEXT LZ_KIND_STRING
#define K_NUMBER LZ_KINDS_NUMBER
#define K_CONVERTIBLE (LZ_KIND_INT | LZ_KIND_FLOAT | LZ_KIND_STRING)

/*
 * Rows of one name must be next to each other: they form one group.
 * Counts are the arguments after the receiver.
 */
static const LzMethod methods[] = {
    /* every kind */
    {"to_string", LZ_KINDS_ANY, lz_method_to_string, 0, 0, LZ_KIND_STRING},
    {"to_bool", LZ_KINDS_ANY, lz_builtin_bool, 0, 0, LZ_KIND_BOOL},
    {"to_int", K_CONVERTIBLE, lz_builtin_int, 0, 0, LZ_KIND_INT},
    {"to_float", K_CONVERTIBLE, lz_builtin_float, 0, 0, LZ_KIND_FLOAT},

    /* string */
    {"to_upper", K_TEXT, lz_method_to_upper, 0, 0, LZ_KIND_STRING},
    {"to_lower", K_TEXT, lz_method_to_lower, 0, 0, LZ_KIND_STRING},
    {"trim", K_TEXT, lz_method_trim, 0, 0, LZ_KIND_STRING},
    {"trim_start", K_TEXT, lz_method_trim_start, 0, 0, LZ_KIND_STRING},
    {"trim_end", K_TEXT, lz_method_trim_end, 0, 0, LZ_KIND_STRING},
    {"split", K_TEXT, lz_method_split, 1, 1, LZ_KIND_ARRAY},
    {"contains", K_TEXT, lz_method_contains, 1, 1, LZ_KIND_BOOL},
    {"starts_with", K_TEXT, lz_method_starts_with, 1, 1, LZ_KIND_BOOL},
    {"ends_with", K_TEXT, lz_method_ends_with, 1, 1, LZ_KIND_BOOL},
    {"replace", K_TEXT, lz_method_replace, 2, 2, LZ_KIND_STRING},
    {"repeat", K_TEXT, lz_method_repeat, 1, 1, LZ_KIND_STRING},
    {"index_of", K_TEXT, lz_method_index_of, 1, 1, LZ_KIND_INT},
    {"chars", K_TEXT, lz_method_chars, 0, 0, LZ_KIND_ARRAY},
    {"slice", K_TEXT, lz_method_slice, 1, 2, LZ_KIND_STRING},

    /* string and array */
    {"is_empty", LZ_KIND_STRING | LZ_KIND_ARRAY, lz_method_is_empty, 0, 0, LZ_KIND_BOOL},

    /* array */
    {"first", LZ_KIND_ARRAY, lz_method_first, 0, 0, LZ_KINDS_ANY},
    {"last", LZ_KIND_ARRAY, lz_method_last, 0, 0, LZ_KINDS_ANY},

    /* number */
    {"abs", K_NUMBER, lz_method_abs, 0, 0, LZ_KINDS_NUMBER},
    {"floor", K_NUMBER, lz_method_floor, 0, 0, LZ_KIND_INT},
    {"ceil", K_NUMBER, lz_method_ceil, 0, 0, LZ_KIND_INT},
    {"round", K_NUMBER, lz_method_round, 0, 0, LZ_KIND_INT},
    {"sqrt", K_NUMBER, lz_method_sqrt, 0, 0, LZ_KIND_FLOAT},
    {"pow", K_NUMBER, lz_method_pow, 1, 1, LZ_KINDS_NUMBER},
    {"min", K_NUMBER, lz_method_min, 1, 1, LZ_KINDS_NUMBER},
    {"max", K_NUMBER, lz_method_max, 1, 1, LZ_KINDS_NUMBER},
};

#define METHOD_COUNT (sizeof methods / sizeof methods[0])

static bool same_group(const LzMethod *a, const LzMethod *b) {
    return strcmp(a->name, b->name) == 0;
}

/* "string", or "string or array": the kinds some row of the group serves. */
static void kinds_served(const LzMethod *first, char *out, size_t size) {
    uint16_t mask = 0;
    for (const LzMethod *m = first; m < methods + METHOD_COUNT && same_group(m, first); m++) {
        mask = (uint16_t)(mask | m->kinds);
    }
    out[0] = '\0';
    size_t used = 0;
    for (uint16_t bit = 1; bit != 0 && bit <= LZ_KIND_ARRAY; bit = (uint16_t)(bit << 1)) {
        if ((mask & bit) == 0 || used + 24 > size) {
            continue;
        }
        used += (size_t)snprintf(out + used, size - used, "%s%s", used > 0 ? " or " : "",
                                 lz_kind_name(bit));
    }
}

static LzStatus arity_error(LzVM *vm, const LzMethod *m, int given) {
    if (m->min_args == m->max_args) {
        return lz_vm_set_error(vm, LZ_ERR_ARITY, "%s(): expected %d argument%s, got %d", m->name,
                               m->min_args, m->min_args == 1 ? "" : "s", given);
    }
    return lz_vm_set_error(vm, LZ_ERR_ARITY, "%s(): expected %d to %d arguments, got %d", m->name,
                           m->min_args, m->max_args, given);
}

/*
 * The one function behind every method name: vm->current_native is the dispatcher
 * that was called, and its data points at the first row of its group.
 */
static LzStatus dispatch(LzVM *vm, int argc, const LzValue *argv, LzValue *result) {
    const LzNative *self = vm->current_native;
    const LzMethod *first = (const LzMethod *)self->data;
    uint16_t kind = lz_kind_of(&argv[0]);

    for (const LzMethod *m = first; m < methods + METHOD_COUNT && same_group(m, first); m++) {
        if ((m->kinds & kind) == 0) {
            continue;
        }
        int given = argc - 1;
        if (given < m->min_args || given > m->max_args) {
            return arity_error(vm, m, given);
        }
        return m->fn(vm, argc, argv, result);
    }

    /* No row for this kind: a function of the same name (`xs.contains(3)`, `xs.min()`). */
    const LzValue *global = lz_vm_lookup(vm, self->name);
    if (global != NULL && (lz_kind_of(global) & LZ_KIND_FUNCTION) != 0) {
        if (global->type == LZ_VAL_OBJECT && global->as.object->type == LZ_OBJ_NATIVE) {
            return lz_native_call(vm, (const LzNative *)global->as.object, argc, argv, result);
        }
        return lz_vm_call_value(vm, global, (size_t)argc, argv, result);
    }

    char served[64];
    kinds_served(first, served, sizeof served);
    return lz_vm_set_error(vm, LZ_ERR_RUNTIME, "%s has no method '%s' (it works on %s)",
                           lz_kind_name(kind), self->name->data, served);
}

LzStatus lz_method_install(LzVM *vm) {
    for (size_t i = 0; i < METHOD_COUNT; i++) {
        if (i > 0 && same_group(&methods[i], &methods[i - 1])) {
            continue;
        }
        const LzMethod *first = &methods[i];

        LzString *name = lz_vm_intern(vm, first->name, strlen(first->name));
        LzNative *native =
            name == NULL ? NULL : lz_native_new(&vm->heap, name, dispatch, 1, LZ_VARIADIC);
        if (native == NULL) {
            return lz_vm_out_of_memory(vm);
        }
        native->data = first;

        /* What a call can return: what the rows return, unless another function can answer too. */
        uint16_t kinds = 0;
        for (const LzMethod *m = first; m < methods + METHOD_COUNT && same_group(m, first); m++) {
            kinds = (uint16_t)(kinds | m->result_kinds);
        }
        native->result_kinds = lz_vm_lookup(vm, name) != NULL ? (uint16_t)LZ_KINDS_ANY : kinds;

        bool defined = lz_vm_define_method(vm, name, lz_value_object(&native->base));
        lz_object_release(&vm->heap, &native->base);
        if (!defined) {
            return lz_vm_out_of_memory(vm);
        }
    }
    return LZ_OK;
}
