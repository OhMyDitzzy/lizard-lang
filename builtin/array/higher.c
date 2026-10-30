#include "array/higher.h"

#include "lizard/array.h"

/*
 * Rules shared by all of them:
 *  - The array is visited from the first item to the one that was last when the call
 *    began (like JavaScript's forEach), so a callback that pushes to the array it is
 *    walking cannot make the loop endless. If the callback shrinks the array, the loop
 *    stops where the array now ends.
 *  - Every item is retained while its callback runs: the callback may change the array.
 *  - The callback gets the item only. Truthiness is the language's: only nil and false are false.
 */

static LzStatus want_array_and_function(LzVM *vm, const char *name, const LzValue *argv,
                                        LzArray **array) {
    if (!lz_value_is_array(&argv[0])) {
        return lz_vm_set_error(vm, LZ_ERR_RUNTIME, "%s(): expected an array, got %s", name,
                               lz_kind_name(lz_kind_of(&argv[0])));
    }
    if ((lz_kind_of(&argv[1]) & LZ_KIND_FUNCTION) == 0) {
        return lz_vm_set_error(vm, LZ_ERR_RUNTIME, "%s(): expected a function, got %s", name,
                               lz_kind_name(lz_kind_of(&argv[1])));
    }
    *array = lz_value_as_array(&argv[0]);
    return lz_vm_charge_fuel(vm, lz_array_cost((*array)->count));
}

/* Calls f(item) for items[i]; *item is the retained item (the caller releases it), *out the result. */
static LzStatus visit(LzVM *vm, const LzArray *array, size_t i, const LzValue *callback,
                      LzValue *item, LzValue *out) {
    *item = array->items[i];
    lz_value_retain(item);
    LzStatus status = lz_vm_call_value(vm, callback, 1, item, out);
    if (status != LZ_OK) {
        lz_value_release(&vm->heap, item);
    }
    return status;
}

LzStatus lz_builtin_map(LzVM *vm, int argc, const LzValue *argv, LzValue *result) {
    (void)argc;
    LzArray *source = NULL;
    LzStatus status = want_array_and_function(vm, "map", argv, &source);
    if (status != LZ_OK) {
        return status;
    }
    size_t total = source->count;
    LzArray *mapped = lz_array_new(&vm->heap, total);
    if (mapped == NULL) {
        return lz_vm_out_of_memory(vm);
    }
    for (size_t i = 0; i < total && i < source->count; i++) {
        LzValue item;
        LzValue value;
        status = visit(vm, source, i, &argv[1], &item, &value);
        if (status != LZ_OK) {
            break;
        }
        lz_value_release(&vm->heap, &item);
        status = lz_array_push(vm, mapped, &value);
        lz_value_release(&vm->heap, &value);
        if (status != LZ_OK) {
            break;
        }
    }
    if (status != LZ_OK) {
        lz_object_release(&vm->heap, &mapped->base);
        return status;
    }
    *result = lz_value_object(&mapped->base);
    return LZ_OK;
}

LzStatus lz_builtin_filter(LzVM *vm, int argc, const LzValue *argv, LzValue *result) {
    (void)argc;
    LzArray *source = NULL;
    LzStatus status = want_array_and_function(vm, "filter", argv, &source);
    if (status != LZ_OK) {
        return status;
    }
    size_t total = source->count;
    LzArray *kept = lz_array_new(&vm->heap, total);
    if (kept == NULL) {
        return lz_vm_out_of_memory(vm);
    }
    kept->element = source->element; /* items of the source already fit its type */
    for (size_t i = 0; i < total && i < source->count; i++) {
        LzValue item;
        LzValue verdict;
        status = visit(vm, source, i, &argv[1], &item, &verdict);
        if (status != LZ_OK) {
            break;
        }
        bool keep = lz_value_is_truthy(&verdict);
        lz_value_release(&vm->heap, &verdict);
        if (keep) {
            lz_array_note_store(kept, &item);
            kept->items[kept->count++] = item; /* the retained reference moves in */
        } else {
            lz_value_release(&vm->heap, &item);
        }
    }
    if (status != LZ_OK) {
        lz_object_release(&vm->heap, &kept->base);
        return status;
    }
    *result = lz_value_object(&kept->base);
    return LZ_OK;
}

LzStatus lz_builtin_reduce(LzVM *vm, int argc, const LzValue *argv, LzValue *result) {
    (void)argc;
    LzArray *source = NULL;
    LzStatus status = want_array_and_function(vm, "reduce", argv, &source);
    if (status != LZ_OK) {
        return status;
    }
    LzValue acc = argv[2];
    lz_value_retain(&acc);
    size_t total = source->count;
    for (size_t i = 0; i < total && i < source->count; i++) {
        LzValue item = source->items[i];
        lz_value_retain(&item);
        LzValue pair[2] = {acc, item};
        LzValue next;
        status = lz_vm_call_value(vm, &argv[1], 2, pair, &next);
        lz_value_release(&vm->heap, &item);
        lz_value_release(&vm->heap, &acc);
        if (status != LZ_OK) {
            return status;
        }
        acc = next;
    }
    *result = acc; /* the accumulator's reference moves to the caller */
    return LZ_OK;
}

LzStatus lz_builtin_each(LzVM *vm, int argc, const LzValue *argv, LzValue *result) {
    (void)argc;
    LzArray *source = NULL;
    LzStatus status = want_array_and_function(vm, "each", argv, &source);
    if (status != LZ_OK) {
        return status;
    }
    size_t total = source->count;
    for (size_t i = 0; i < total && i < source->count; i++) {
        LzValue item;
        LzValue ignored;
        status = visit(vm, source, i, &argv[1], &item, &ignored);
        if (status != LZ_OK) {
            return status;
        }
        lz_value_release(&vm->heap, &item);
        lz_value_release(&vm->heap, &ignored);
    }
    *result = lz_value_nil();
    return LZ_OK;
}

/* find, any and all differ only in what a verdict means and what is returned. */
typedef enum Search { SEARCH_FIND, SEARCH_ANY, SEARCH_ALL } Search;

static LzStatus search(LzVM *vm, const char *name, Search mode, const LzValue *argv,
                       LzValue *result) {
    LzArray *source = NULL;
    LzStatus status = want_array_and_function(vm, name, argv, &source);
    if (status != LZ_OK) {
        return status;
    }
    size_t total = source->count;
    for (size_t i = 0; i < total && i < source->count; i++) {
        LzValue item;
        LzValue verdict;
        status = visit(vm, source, i, &argv[1], &item, &verdict);
        if (status != LZ_OK) {
            return status;
        }
        bool truthy = lz_value_is_truthy(&verdict);
        lz_value_release(&vm->heap, &verdict);
        if (mode == SEARCH_FIND && truthy) {
            *result = item; /* the retained reference moves to the caller */
            return LZ_OK;
        }
        lz_value_release(&vm->heap, &item);
        if (mode == SEARCH_ANY && truthy) {
            *result = lz_value_bool(true);
            return LZ_OK;
        }
        if (mode == SEARCH_ALL && !truthy) {
            *result = lz_value_bool(false);
            return LZ_OK;
        }
    }
    *result = mode == SEARCH_FIND ? lz_value_nil() : lz_value_bool(mode == SEARCH_ALL);
    return LZ_OK;
}

LzStatus lz_builtin_find(LzVM *vm, int argc, const LzValue *argv, LzValue *result) {
    (void)argc;
    return search(vm, "find", SEARCH_FIND, argv, result);
}

LzStatus lz_builtin_any(LzVM *vm, int argc, const LzValue *argv, LzValue *result) {
    (void)argc;
    return search(vm, "any", SEARCH_ANY, argv, result);
}

LzStatus lz_builtin_all(LzVM *vm, int argc, const LzValue *argv, LzValue *result) {
    (void)argc;
    return search(vm, "all", SEARCH_ALL, argv, result);
}
