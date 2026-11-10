#include "method/method.h"

#include <stdio.h>
#include <string.h>

#include "array/array.h"
#include "array/higher.h"
#include "convert/convert.h"
#include "lizard/array.h"
#include "lizard/class.h"
#include "lizard/dict.h"
#include "lizard/format.h"
#include "lizard/link.h"

/* ---- methods that need no file of their own ---- */

LzStatus lz_method_to_string(LzVM *vm, int argc, const LzValue *argv, LzValue *result) {
    return lz_builtin_string(vm, argc, argv, result);
}

static LzStatus string_result(LzVM *vm, LzBuffer *text, LzValue *result) {
    LzString *string = lz_string_new(&vm->heap, text->data, text->length);
    lz_buffer_free(text);
    if (string == NULL) {
        return lz_vm_out_of_memory(vm);
    }
    *result = lz_value_object(&string->base);
    return LZ_OK;
}

/* x.format(spec) is `{x:spec}`. */
LzStatus lz_method_format(LzVM *vm, int argc, const LzValue *argv, LzValue *result) {
    (void)argc;
    if (argv[1].type != LZ_VAL_OBJECT || argv[1].as.object->type != LZ_OBJ_STRING) {
        return lz_vm_set_error(vm, LZ_ERR_RUNTIME, "format(): the spec must be a string, got %s",
                               lz_value_kind_name(&argv[1]));
    }
    const LzString *spec_text = (const LzString *)argv[1].as.object;
    LzFormatSpec spec;
    LzFormatError error;
    if (!lz_format_parse(spec_text->data, spec_text->length, &spec, &error)) {
        return lz_vm_set_error(vm, LZ_ERR_RUNTIME, "format(): invalid spec \"%s\": %s",
                               spec_text->data, error.message);
    }
    LzBuffer text = {0};
    LzWriter writer = lz_writer_from_buffer(&text);
    LzFormatResult done = lz_format_value(&argv[0], &spec, &writer, &error);
    if (done == LZ_FORMAT_BAD) {
        lz_buffer_free(&text);
        return lz_vm_set_error(vm, LZ_ERR_RUNTIME, "format(): %s", error.message);
    }
    if (done != LZ_FORMAT_OK) {
        lz_buffer_free(&text);
        LzStatus text_status = LZ_OK;
        if (lz_vm_take_text_status(vm, &text_status)) {
            return text_status;
        }
        return lz_vm_out_of_memory(vm);
    }
    return string_result(vm, &text, result);
}

/* x.to_debug() is `{x=}` without the name: strings are quoted. */
LzStatus lz_method_to_debug(LzVM *vm, int argc, const LzValue *argv, LzValue *result) {
    (void)argc;
    LzBuffer text = {0};
    LzWriter writer = lz_writer_from_buffer(&text);
    if (!lz_value_write_debug(&argv[0], &writer)) {
        lz_buffer_free(&text);
        LzStatus text_status = LZ_OK;
        if (lz_vm_take_text_status(vm, &text_status)) {
            return text_status;
        }
        return lz_vm_out_of_memory(vm);
    }
    return string_result(vm, &text, result);
}

LzStatus lz_method_is_empty(LzVM *vm, int argc, const LzValue *argv, LzValue *result) {
    (void)vm;
    (void)argc;
    if (lz_value_is_array(&argv[0])) {
        *result = lz_value_bool(lz_value_as_array(&argv[0])->count == 0);
    } else if (lz_value_is_dict(&argv[0])) {
        *result = lz_value_bool(lz_value_as_dict(&argv[0])->count == 0);
    } else {
        *result = lz_value_bool(((const LzString *)argv[0].as.object)->length == 0);
    }
    return LZ_OK;
}

static LzStatus end_item(LzVM *vm, const char *name, const LzValue *argv, bool last, LzValue *result) {
    if (!lz_value_is_array(&argv[0])) {
        return lz_vm_set_error(vm, LZ_ERR_RUNTIME, "%s(): expected an array, got %s", name,
                               lz_value_kind_name(&argv[0]));
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
    {"to_debug", LZ_KINDS_ANY, lz_method_to_debug, 0, 0, LZ_KIND_STRING},
    {"format", LZ_KINDS_ANY, lz_method_format, 1, 1, LZ_KIND_STRING},
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
    {"contains", LZ_KIND_ARRAY, lz_builtin_contains, 1, 1, LZ_KIND_BOOL},
    {"starts_with", K_TEXT, lz_method_starts_with, 1, 1, LZ_KIND_BOOL},
    {"ends_with", K_TEXT, lz_method_ends_with, 1, 1, LZ_KIND_BOOL},
    {"replace", K_TEXT, lz_method_replace, 2, 2, LZ_KIND_STRING},
    {"repeat", K_TEXT, lz_method_repeat, 1, 1, LZ_KIND_STRING},
    {"index_of", K_TEXT, lz_method_index_of, 1, 1, LZ_KIND_INT},
    {"index_of", LZ_KIND_ARRAY, lz_builtin_index_of, 1, 1, LZ_KIND_INT},
    {"chars", K_TEXT, lz_method_chars, 0, 0, LZ_KIND_ARRAY},
    {"slice", K_TEXT, lz_method_slice, 1, 2, LZ_KIND_STRING},

    /* string, array and dict */
    {"is_empty", LZ_KIND_STRING | LZ_KIND_ARRAY | LZ_KIND_DICT, lz_method_is_empty, 0, 0,
     LZ_KIND_BOOL},

    /* array */
    {"first", LZ_KIND_ARRAY, lz_method_first, 0, 0, LZ_KINDS_ANY},
    {"last", LZ_KIND_ARRAY, lz_method_last, 0, 0, LZ_KINDS_ANY},
    {"push", LZ_KIND_ARRAY, lz_builtin_push, 1, 1, LZ_KIND_NIL},
    {"pop", LZ_KIND_ARRAY, lz_builtin_pop, 0, 0, LZ_KINDS_ANY},
    {"insert", LZ_KIND_ARRAY, lz_builtin_insert, 2, 2, LZ_KIND_NIL},
    {"reverse", LZ_KIND_ARRAY, lz_builtin_reverse, 0, 0, LZ_KIND_NIL},
    {"sort", LZ_KIND_ARRAY, lz_builtin_sort, 0, 0, LZ_KIND_NIL},
    {"join", LZ_KIND_ARRAY, lz_builtin_join, 1, 1, LZ_KIND_STRING},
    {"sum", LZ_KIND_ARRAY, lz_builtin_sum, 0, 0, LZ_KINDS_NUMBER},
    {"map", LZ_KIND_ARRAY, lz_builtin_map, 1, 1, LZ_KIND_ARRAY},
    {"filter", LZ_KIND_ARRAY, lz_builtin_filter, 1, 1, LZ_KIND_ARRAY},
    {"reduce", LZ_KIND_ARRAY, lz_builtin_reduce, 2, 2, LZ_KINDS_ANY},
    {"each", LZ_KIND_ARRAY, lz_builtin_each, 1, 1, LZ_KIND_NIL},
    {"find", LZ_KIND_ARRAY, lz_builtin_find, 1, 1, LZ_KINDS_ANY},
    {"any", LZ_KIND_ARRAY, lz_builtin_any, 1, 1, LZ_KIND_BOOL},
    {"all", LZ_KIND_ARRAY, lz_builtin_all, 1, 1, LZ_KIND_BOOL},

    /* number */
    {"abs", K_NUMBER, lz_method_abs, 0, 0, LZ_KINDS_NUMBER},
    {"floor", K_NUMBER, lz_method_floor, 0, 0, LZ_KIND_INT},
    {"ceil", K_NUMBER, lz_method_ceil, 0, 0, LZ_KIND_INT},
    {"round", K_NUMBER, lz_method_round, 0, 0, LZ_KIND_INT},
    {"sqrt", K_NUMBER, lz_method_sqrt, 0, 0, LZ_KIND_FLOAT},
    {"pow", K_NUMBER, lz_method_pow, 1, 1, LZ_KINDS_NUMBER},
    {"min", K_NUMBER, lz_method_min, 1, 1, LZ_KINDS_NUMBER},
    {"min", LZ_KIND_ARRAY, lz_builtin_min, 0, 0, LZ_KINDS_NUMBER},
    {"max", K_NUMBER, lz_method_max, 1, 1, LZ_KINDS_NUMBER},
    {"max", LZ_KIND_ARRAY, lz_builtin_max, 0, 0, LZ_KINDS_NUMBER},

    /* dict */
    {"len", LZ_KIND_STRING | LZ_KIND_ARRAY, lz_builtin_len, 0, 0, LZ_KIND_INT},
    {"len", LZ_KIND_DICT, lz_method_dict_len, 0, 0, LZ_KIND_INT},
    {"keys", LZ_KIND_DICT, lz_method_dict_keys, 0, 0, LZ_KIND_ARRAY},
    {"values", LZ_KIND_DICT, lz_method_dict_values, 0, 0, LZ_KIND_ARRAY},
    {"items", LZ_KIND_DICT, lz_method_dict_items, 0, 0, LZ_KIND_ARRAY},
    {"get", LZ_KIND_DICT, lz_method_dict_get, 1, 2, LZ_KINDS_ANY},
    {"has", LZ_KIND_DICT, lz_method_dict_has, 1, 1, LZ_KIND_BOOL},
    {"remove", LZ_KIND_DICT, lz_method_dict_remove, 1, 1, LZ_KINDS_ANY},
    {"remove", LZ_KIND_ARRAY, lz_builtin_remove, 1, 1, LZ_KINDS_ANY},
    {"clear", LZ_KIND_DICT, lz_method_dict_clear, 0, 0, LZ_KIND_NIL},
    {"clear", LZ_KIND_ARRAY, lz_builtin_clear, 0, 0, LZ_KIND_NIL},
    {"copy", LZ_KIND_DICT, lz_method_dict_copy, 0, 0, LZ_KIND_DICT},
    {"copy", LZ_KIND_ARRAY, lz_builtin_copy, 0, 0, LZ_KIND_ARRAY},
    {"merge", LZ_KIND_DICT, lz_method_dict_merge, 1, 1, LZ_KIND_DICT},

    /* int only */
    {"bit_count", LZ_KIND_INT, lz_method_bit_count, 0, 0, LZ_KIND_INT},
    {"bit_length", LZ_KIND_INT, lz_method_bit_length, 0, 0, LZ_KIND_INT},
    {"bit", LZ_KIND_INT, lz_method_bit, 1, 1, LZ_KIND_BOOL},
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
    for (uint16_t bit = 1; bit != 0 && bit <= LZ_KIND_DICT; bit = (uint16_t)(bit << 1)) {
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
    if (lz_value_is_link(&argv[0])) {
        /* Only the methods every value has (to_string, to_debug) take a link cell; they are
         * marked takes_links, and every row of theirs serves every kind, nil included. */
        kind = LZ_KIND_NIL;
    }

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

    /* No row for this kind. There is no fallback to a function of the same name: a method is
     * found in the type of the value and nowhere else. */
    const LzValue *global = lz_vm_lookup(vm, self->name);
    /* nil has no methods at all, which says more than naming a function. */
    if (kind != LZ_KIND_NIL && global != NULL && (lz_kind_of(global) & LZ_KIND_FUNCTION) != 0) {
        return lz_vm_not_a_method_error(vm, self->name->data, lz_kind_name(kind));
    }

    char served[64];
    kinds_served(first, served, sizeof served);
    return lz_vm_set_error(vm, LZ_ERR_RUNTIME, "%s has no method '%s' (it works on %s)",
                           lz_kind_name(kind), self->name->data, served);
}

bool lz_method_has_row(const char *name, unsigned kinds) {
    for (size_t i = 0; i < METHOD_COUNT; i++) {
        if ((methods[i].kinds & kinds) != 0 && strcmp(methods[i].name, name) == 0) {
            return true;
        }
    }
    return false;
}

LzStatus lz_method_install(LzVM *vm) {
    vm->method_row_check = lz_method_has_row;
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
        native->takes_links = (strcmp(first->name, "to_string") == 0 || strcmp(first->name, "to_debug") == 0)
                                  ? LZ_LINKS_ANY
                              : (strcmp(first->name, "push") == 0 || strcmp(first->name, "insert") == 0)
                                  ? LZ_LINKS_ITEMS
                                  : LZ_LINKS_NONE;

        /* What a call can return: what the rows return. */
        uint16_t kinds = 0;
        for (const LzMethod *m = first; m < methods + METHOD_COUNT && same_group(m, first); m++) {
            kinds = (uint16_t)(kinds | m->result_kinds);
        }
        native->result_kinds = kinds;

        bool defined = lz_vm_define_method(vm, name, lz_value_object(&native->base));
        lz_object_release(&vm->heap, &native->base);
        if (!defined) {
            return lz_vm_out_of_memory(vm);
        }
    }
    return LZ_OK;
}
