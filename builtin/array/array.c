#include "array/array.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lizard/array.h"
#include "lizard/class.h"
#include "lizard/dict.h"

static const char *kind_of(const LzValue *value) {
    return lz_value_kind_name(value);
}

static bool is_string(const LzValue *value) {
    return value->type == LZ_VAL_OBJECT && value->as.object->type == LZ_OBJ_STRING;
}

static bool is_number(const LzValue *value) {
    return value->type == LZ_VAL_INT || value->type == LZ_VAL_FLOAT;
}

/* argv[0] must be an array; reports `name(): expected an array, got T` otherwise. */
static LzStatus want_array(LzVM *vm, const char *name, const LzValue *value, LzArray **out) {
    if (!lz_value_is_array(value)) {
        return lz_vm_set_error(vm, LZ_ERR_RUNTIME, "%s(): expected an array, got %s", name,
                               kind_of(value));
    }
    *out = lz_value_as_array(value);
    return LZ_OK;
}

static LzStatus want_int(LzVM *vm, const char *name, const LzValue *value, int64_t *out) {
    if (value->type != LZ_VAL_INT) {
        return lz_vm_set_error(vm, LZ_ERR_RUNTIME, "%s(): expected an int, got %s", name,
                               kind_of(value));
    }
    *out = value->as.integer;
    return LZ_OK;
}

/* Wraps lz_array_resolve_index so the message names the function. */
static LzStatus want_index(LzVM *vm, const char *name, const LzValue *index, size_t count,
                           size_t *out) {
    LzStatus status = lz_array_resolve_index(vm, index, count, out);
    if (status == LZ_ERR_RUNTIME) {
        char message[sizeof vm->error];
        (void)snprintf(message, sizeof message, "%s", vm->error);
        return lz_vm_set_error(vm, status, "%s(): %s", name, message);
    }
    return status;
}

LzStatus lz_builtin_len(LzVM *vm, int argc, const LzValue *argv, LzValue *result) {
    (void)argc;
    if (lz_value_is_array(&argv[0])) {
        *result = lz_value_int((int64_t)lz_value_as_array(&argv[0])->count);
        return LZ_OK;
    }
    if (lz_value_is_dict(&argv[0])) {
        *result = lz_value_int((int64_t)lz_value_as_dict(&argv[0])->count);
        return LZ_OK;
    }
    if (is_string(&argv[0])) {
        /* Length in characters (UTF-8 code points): bytes that are not continuation bytes. */
        const LzString *text = (const LzString *)argv[0].as.object;
        LzStatus charged = lz_vm_charge_fuel(vm, text->length / 64);
        if (charged != LZ_OK) {
            return charged;
        }
        int64_t length = 0;
        for (size_t i = 0; i < text->length; i++) {
            if (((unsigned char)text->data[i] & 0xC0) != 0x80) {
                length++;
            }
        }
        *result = lz_value_int(length);
        return LZ_OK;
    }
    return lz_vm_set_error(vm, LZ_ERR_RUNTIME, "len(): expected an array or string, got %s",
                           kind_of(&argv[0]));
}

LzStatus lz_builtin_push(LzVM *vm, int argc, const LzValue *argv, LzValue *result) {
    (void)argc;
    LzArray *array = NULL;
    LzStatus status = want_array(vm, "push", &argv[0], &array);
    if (status == LZ_OK) {
        status = lz_array_push(vm, array, &argv[1]);
    }
    if (status == LZ_OK) {
        *result = lz_value_nil();
    }
    return status;
}

LzStatus lz_builtin_pop(LzVM *vm, int argc, const LzValue *argv, LzValue *result) {
    (void)argc;
    LzArray *array = NULL;
    LzStatus status = want_array(vm, "pop", &argv[0], &array);
    if (status != LZ_OK) {
        return status;
    }
    if (array->count == 0) {
        return lz_vm_set_error(vm, LZ_ERR_RUNTIME, "pop(): array is empty");
    }
    lz_array_remove_at(array, array->count - 1,
                       result); /* the array's reference moves to the caller */
    return LZ_OK;
}

LzStatus lz_builtin_insert(LzVM *vm, int argc, const LzValue *argv, LzValue *result) {
    (void)argc;
    LzArray *array = NULL;
    LzStatus status = want_array(vm, "insert", &argv[0], &array);
    if (status != LZ_OK) {
        return status;
    }
    int64_t position = 0;
    status = want_int(vm, "insert", &argv[1], &position);
    if (status != LZ_OK) {
        return status;
    }
    int64_t count = (int64_t)array->count;
    int64_t index = position < 0 ? position + count : position;
    if (index < 0 || index > count) {
        return lz_vm_set_error(vm, LZ_ERR_RUNTIME,
                               "insert(): index %lld out of range for array of length %zu",
                               (long long)position, array->count);
    }
    status = lz_array_check_store(vm, array, &argv[2]);
    if (status != LZ_OK) {
        return status;
    }
    if (array->count >= LZ_ARRAY_MAX_LENGTH) {
        return lz_vm_set_error(vm, LZ_ERR_RUNTIME, "array too large (limit %zu items)",
                               (size_t)LZ_ARRAY_MAX_LENGTH);
    }
    status = lz_vm_charge_fuel(vm, lz_array_cost(array->count - (size_t)index));
    if (status != LZ_OK) {
        return status;
    }
    if (!lz_array_insert_at(&vm->heap, array, (size_t)index, &argv[2])) {
        return lz_vm_out_of_memory(vm);
    }
    *result = lz_value_nil();
    return LZ_OK;
}

LzStatus lz_builtin_remove(LzVM *vm, int argc, const LzValue *argv, LzValue *result) {
    (void)argc;
    LzArray *array = NULL;
    LzStatus status = want_array(vm, "remove", &argv[0], &array);
    size_t index = 0;
    if (status == LZ_OK) {
        status = want_index(vm, "remove", &argv[1], array->count, &index);
    }
    if (status == LZ_OK) {
        status = lz_vm_charge_fuel(vm, lz_array_cost(array->count - index));
    }
    if (status != LZ_OK) {
        return status;
    }
    lz_array_remove_at(array, index, result); /* returns the removed item */
    return LZ_OK;
}

LzStatus lz_builtin_clear(LzVM *vm, int argc, const LzValue *argv, LzValue *result) {
    (void)argc;
    LzArray *array = NULL;
    LzStatus status = want_array(vm, "clear", &argv[0], &array);
    if (status == LZ_OK) {
        status = lz_vm_charge_fuel(vm, lz_array_cost(array->count));
    }
    if (status != LZ_OK) {
        return status;
    }
    lz_array_clear(&vm->heap, array);
    *result = lz_value_nil();
    return LZ_OK;
}

LzStatus lz_builtin_copy(LzVM *vm, int argc, const LzValue *argv, LzValue *result) {
    (void)argc;
    LzArray *array = NULL;
    LzStatus status = want_array(vm, "copy", &argv[0], &array);
    return status != LZ_OK ? status : lz_array_copy(vm, array, result);
}

/* Index of the first item equal to value, or -1. */
static LzStatus find(LzVM *vm, const char *name, const LzValue *argv, int64_t *found) {
    LzArray *array = NULL;
    LzStatus status = want_array(vm, name, &argv[0], &array);
    if (status != LZ_OK) {
        return status;
    }
    status = lz_vm_charge_fuel(vm, lz_array_cost(array->count));
    if (status != LZ_OK) {
        return status;
    }
    *found = -1;
    for (size_t i = 0; i < array->count && *found < 0; i++) {
        bool same = false;
        status = lz_vm_values_equal(vm, &array->items[i], &argv[1], &same);
        if (status != LZ_OK) {
            return status;
        }
        if (same) {
            *found = (int64_t)i;
        }
    }
    return LZ_OK;
}

LzStatus lz_builtin_contains(LzVM *vm, int argc, const LzValue *argv, LzValue *result) {
    (void)argc;
    int64_t found = -1;
    LzStatus status = find(vm, "contains", argv, &found);
    if (status == LZ_OK) {
        *result = lz_value_bool(found >= 0);
    }
    return status;
}

LzStatus lz_builtin_index_of(LzVM *vm, int argc, const LzValue *argv, LzValue *result) {
    (void)argc;
    int64_t found = -1;
    LzStatus status = find(vm, "index_of", argv, &found);
    if (status == LZ_OK) {
        *result = lz_value_int(found);
    }
    return status;
}

LzStatus lz_builtin_reverse(LzVM *vm, int argc, const LzValue *argv, LzValue *result) {
    (void)argc;
    LzArray *array = NULL;
    LzStatus status = want_array(vm, "reverse", &argv[0], &array);
    if (status == LZ_OK) {
        status = lz_vm_charge_fuel(vm, lz_array_cost(array->count));
    }
    if (status != LZ_OK) {
        return status;
    }
    for (size_t i = 0, j = array->count; i + 1 < j; i++, j--) {
        LzValue swap = array->items[i];
        array->items[i] = array->items[j - 1];
        array->items[j - 1] = swap;
    }
    *result = lz_value_nil();
    return LZ_OK;
}

/* sort */

static int compare_items(const LzValue *a, const LzValue *b) {
    if (is_string(a)) {
        const LzString *x = (const LzString *)a->as.object;
        const LzString *y = (const LzString *)b->as.object;
        size_t common = x->length < y->length ? x->length : y->length;
        int order = common == 0 ? 0 : memcmp(x->data, y->data, common);
        if (order != 0) {
            return order;
        }
        return x->length == y->length ? 0 : (x->length < y->length ? -1 : 1);
    }
    return (int)lz_number_order(a, b);
}

/* Stable bottom-up merge sort of items[0..count) using tmp as scratch. */
static void merge_sort(LzValue *items, LzValue *tmp, size_t count) {
    for (size_t width = 1; width < count; width *= 2) {
        for (size_t lo = 0; lo < count; lo += 2 * width) {
            size_t mid = lo + width < count ? lo + width : count;
            size_t hi = lo + 2 * width < count ? lo + 2 * width : count;
            size_t i = lo;
            size_t j = mid;
            size_t k = lo;
            while (i < mid && j < hi) {
                tmp[k++] = compare_items(&items[j], &items[i]) < 0 ? items[j++] : items[i++];
            }
            while (i < mid) {
                tmp[k++] = items[i++];
            }
            while (j < hi) {
                tmp[k++] = items[j++];
            }
        }
        memcpy(items, tmp, count * sizeof *items);
    }
}

/* `a < b` for two values, through the `operator <` of the objects. */
static LzStatus instance_less(LzVM *vm, const LzValue *a, const LzValue *b, bool *answer) {
    LzValue verdict = lz_value_nil();
    bool handled = false;
    LzStatus status = lz_class_order_operator(vm, LZ_OP_LESS, a, b, &verdict, &handled);
    if (status == LZ_OK) {
        *answer = verdict.as.boolean;
    }
    return status;
}

static bool all_instances(const LzArray *array) {
    for (size_t i = 0; i < array->count; i++) {
        if (!lz_value_is_instance(&array->items[i])) {
            return false;
        }
    }
    return array->count > 0;
}

/*
 * Sorts objects with their class's `operator <`. The work is done on a copy that holds its own
 * references (the operator is code and may change the array); the array is replaced only when the
 * whole sort succeeded, so an error leaves it as it was. Stable, like the sort of numbers.
 */
static LzStatus sort_instances(LzVM *vm, LzArray *array, LzValue *result) {
    size_t count = array->count;
    uint64_t passes = 1;
    for (size_t width = 1; width < count; width *= 2) {
        passes++;
    }
    LzStatus status = lz_vm_charge_fuel(vm, lz_array_cost(count) * passes);
    if (status != LZ_OK) {
        return status;
    }
    size_t bytes = count * sizeof(LzValue) * 2;
    if (!lz_heap_charge(&vm->heap, bytes)) {
        return lz_vm_out_of_memory(vm);
    }
    LzValue *work = malloc(count * sizeof(LzValue) * 2);
    if (work == NULL) {
        lz_heap_uncharge(&vm->heap, bytes);
        return lz_vm_out_of_memory(vm);
    }
    LzValue *tmp = work + count;
    for (size_t i = 0; i < count; i++) {
        work[i] = array->items[i];
        lz_value_retain(&work[i]);
    }
    for (size_t width = 1; width < count && status == LZ_OK; width *= 2) {
        for (size_t lo = 0; lo < count && status == LZ_OK; lo += 2 * width) {
            size_t mid = lo + width < count ? lo + width : count;
            size_t hi = lo + 2 * width < count ? lo + 2 * width : count;
            size_t i = lo;
            size_t j = mid;
            size_t k = lo;
            while (i < mid && j < hi && status == LZ_OK) {
                bool right_first = false;
                status = instance_less(vm, &work[j], &work[i], &right_first);
                if (status == LZ_OK) {
                    tmp[k++] = right_first ? work[j++] : work[i++];
                }
            }
            if (status != LZ_OK) {
                break;
            }
            while (i < mid) {
                tmp[k++] = work[i++];
            }
            while (j < hi) {
                tmp[k++] = work[j++];
            }
        }
        if (status == LZ_OK) {
            memcpy(work, tmp, count * sizeof *work);
        }
    }
    if (status == LZ_OK && array->count != count) {
        status = lz_vm_set_error(vm, LZ_ERR_RUNTIME, "sort(): the array changed while it was being sorted");
    }
    if (status == LZ_OK) {
        for (size_t i = 0; i < count; i++) {
            lz_value_release(&vm->heap, &array->items[i]);
            array->items[i] = work[i]; /* the reference the copy held moves into the array */
        }
        *result = lz_value_nil();
    } else {
        for (size_t i = 0; i < count; i++) {
            lz_value_release(&vm->heap, &work[i]);
        }
    }
    free(work);
    lz_heap_uncharge(&vm->heap, bytes);
    return status;
}

LzStatus lz_builtin_sort(LzVM *vm, int argc, const LzValue *argv, LzValue *result) {
    (void)argc;
    LzArray *array = NULL;
    LzStatus status = want_array(vm, "sort", &argv[0], &array);
    if (status != LZ_OK) {
        return status;
    }
    size_t count = array->count;
    if (all_instances(array)) {
        return sort_instances(vm, array, result);
    }
    if (count < 2) {
        if (count == 1 && !is_number(&array->items[0]) && !is_string(&array->items[0])) {
            return lz_vm_set_error(vm, LZ_ERR_RUNTIME, "sort(): cannot sort values of type %s",
                                   kind_of(&array->items[0]));
        }
        *result = lz_value_nil();
        return LZ_OK;
    }

    /* Everything must be a number, or everything a string. Checked first, so a failure changes
     * nothing. */
    const LzValue *first = &array->items[0];
    bool numbers = is_number(first);
    if (!numbers && !is_string(first)) {
        return lz_vm_set_error(vm, LZ_ERR_RUNTIME, "sort(): cannot sort values of type %s",
                               kind_of(first));
    }
    for (size_t i = 0; i < count; i++) {
        const LzValue *item = &array->items[i];
        if (numbers ? !is_number(item) : !is_string(item)) {
            return lz_vm_set_error(vm, LZ_ERR_RUNTIME,
                                   "sort(): cannot sort mixed types (%s and %s)", kind_of(first),
                                   kind_of(item));
        }
        if (item->type == LZ_VAL_FLOAT && item->as.number != item->as.number) {
            return lz_vm_set_error(vm, LZ_ERR_RUNTIME, "sort(): cannot sort nan");
        }
    }

    uint64_t passes = 1;
    for (size_t width = 1; width < count; width *= 2) {
        passes++;
    }
    status = lz_vm_charge_fuel(vm, lz_array_cost(count) * passes);
    if (status != LZ_OK) {
        return status;
    }
    size_t bytes = count * sizeof(LzValue);
    if (!lz_heap_charge(&vm->heap, bytes)) {
        return lz_vm_out_of_memory(vm);
    }
    LzValue *scratch = malloc(bytes);
    if (scratch == NULL) {
        lz_heap_uncharge(&vm->heap, bytes);
        return lz_vm_out_of_memory(vm);
    }
    merge_sort(array->items, scratch, count);
    free(scratch);
    lz_heap_uncharge(&vm->heap, bytes);
    *result = lz_value_nil();
    return LZ_OK;
}

LzStatus lz_builtin_join(LzVM *vm, int argc, const LzValue *argv, LzValue *result) {
    (void)argc;
    LzArray *array = NULL;
    LzStatus status = want_array(vm, "join", &argv[0], &array);
    if (status != LZ_OK) {
        return status;
    }
    if (!is_string(&argv[1])) {
        return lz_vm_set_error(vm, LZ_ERR_RUNTIME, "join(): separator must be a string, got %s",
                               kind_of(&argv[1]));
    }
    const LzString *separator = (const LzString *)argv[1].as.object;
    status = lz_vm_charge_fuel(vm, lz_array_cost(array->count));
    if (status != LZ_OK) {
        return status;
    }

    LzBuffer text = {0};
    LzWriter writer = lz_writer_from_buffer(&text);
    bool ok = true;
    for (size_t i = 0; i < array->count && ok; i++) {
        if (i > 0) {
            ok = lz_writer_write(&writer, separator->data, separator->length);
        }
        ok = ok && lz_value_write(&array->items[i], &writer);
    }
    if (!ok) {
        lz_buffer_free(&text);
        LzStatus text_status = LZ_OK;
        if (lz_vm_take_text_status(vm, &text_status)) {
            return text_status;
        }
        return lz_vm_out_of_memory(vm);
    }
    status = lz_vm_charge_fuel(vm, text.length / 64);
    LzString *joined = NULL;
    if (status == LZ_OK) {
        joined = lz_string_new(&vm->heap, text.data != NULL ? text.data : "", text.length);
        if (joined == NULL) {
            status = lz_vm_out_of_memory(vm);
        }
    }
    lz_buffer_free(&text);
    if (status == LZ_OK) {
        *result = lz_value_object(&joined->base);
    }
    return status;
}

LzStatus lz_builtin_range(LzVM *vm, int argc, const LzValue *argv, LzValue *result) {
    int64_t from = 0;
    int64_t to = 0;
    LzStatus status =
        argc == 1 ? want_int(vm, "range", &argv[0], &to) : want_int(vm, "range", &argv[0], &from);
    if (status == LZ_OK && argc == 2) {
        status = want_int(vm, "range", &argv[1], &to);
    }
    if (status != LZ_OK) {
        return status;
    }

    uint64_t count = to > from ? (uint64_t)to - (uint64_t)from : 0;
    if (count > LZ_ARRAY_MAX_LENGTH) {
        return lz_vm_set_error(vm, LZ_ERR_RUNTIME, "range(): too many items (limit %zu)",
                               (size_t)LZ_ARRAY_MAX_LENGTH);
    }
    status = lz_vm_charge_fuel(vm, lz_array_cost((size_t)count));
    if (status != LZ_OK) {
        return status;
    }
    LzArray *array = lz_array_new(&vm->heap, (size_t)count);
    if (array == NULL) {
        return lz_vm_out_of_memory(vm);
    }
    for (uint64_t i = 0; i < count; i++) {
        array->items[array->count++] = lz_value_int((int64_t)((uint64_t)from + i));
    }
    *result = lz_value_object(&array->base);
    return LZ_OK;
}

/* sum, min, max: arrays of numbers only */

static LzStatus number_array(LzVM *vm, const char *name, const LzValue *value, LzArray **out) {
    LzStatus status = want_array(vm, name, value, out);
    if (status != LZ_OK) {
        return status;
    }
    for (size_t i = 0; i < (*out)->count; i++) {
        if (!is_number(&(*out)->items[i])) {
            return lz_vm_set_error(vm, LZ_ERR_RUNTIME,
                                   "%s(): expected an array of numbers, got %s at index %zu", name,
                                   kind_of(&(*out)->items[i]), i);
        }
    }
    return lz_vm_charge_fuel(vm, lz_array_cost((*out)->count));
}

LzStatus lz_builtin_sum(LzVM *vm, int argc, const LzValue *argv, LzValue *result) {
    (void)argc;
    LzArray *array = NULL;
    LzStatus status = number_array(vm, "sum", &argv[0], &array);
    if (status != LZ_OK) {
        return status;
    }

    int64_t total = 0;
    double real = 0;
    bool is_float = false;
    for (size_t i = 0; i < array->count; i++) {
        const LzValue *item = &array->items[i];
        if (!is_float && item->type == LZ_VAL_FLOAT) {
            is_float = true;
            real = (double)total;
        }
        if (is_float) {
            real += item->type == LZ_VAL_INT ? (double)item->as.integer : item->as.number;
            continue;
        }
        int64_t value = item->as.integer;
        if ((value > 0 && total > INT64_MAX - value) || (value < 0 && total < INT64_MIN - value)) {
            return lz_vm_set_error(vm, LZ_ERR_RUNTIME, "sum(): integer overflow");
        }
        total += value;
    }
    *result = is_float ? lz_value_float(real) : lz_value_int(total);
    return LZ_OK;
}

/* min or max of objects: only their `operator <` is used (a > b is b < a). */
static LzStatus extreme_instances(LzVM *vm, LzArray *array, bool want_max, LzValue *result) {
    LzStatus status = lz_vm_charge_fuel(vm, lz_array_cost(array->count));
    if (status != LZ_OK) {
        return status;
    }
    LzValue best = array->items[0];
    lz_value_retain(&best);
    for (size_t i = 1; i < array->count && status == LZ_OK; i++) {
        LzValue item = array->items[i];
        lz_value_retain(&item);
        bool better = false;
        status = want_max ? instance_less(vm, &best, &item, &better) : instance_less(vm, &item, &best, &better);
        if (status == LZ_OK && better) {
            lz_value_release(&vm->heap, &best);
            best = item;
        } else {
            lz_value_release(&vm->heap, &item);
        }
    }
    if (status != LZ_OK) {
        lz_value_release(&vm->heap, &best);
        return status;
    }
    *result = best; /* the reference taken above goes to the caller */
    return LZ_OK;
}

static LzStatus extreme(LzVM *vm, const char *name, const LzValue *argv, LzOrder want,
                        LzValue *result) {
    LzArray *array = NULL;
    LzStatus status = want_array(vm, name, &argv[0], &array);
    if (status == LZ_OK && all_instances(array)) {
        return extreme_instances(vm, array, want == LZ_ORDER_GREATER, result);
    }
    if (status != LZ_OK) {
        return status;
    }
    status = number_array(vm, name, &argv[0], &array);
    if (status != LZ_OK) {
        return status;
    }
    if (array->count == 0) {
        return lz_vm_set_error(vm, LZ_ERR_RUNTIME, "%s(): array is empty", name);
    }
    const LzValue *best = NULL;
    for (size_t i = 0; i < array->count; i++) {
        const LzValue *item = &array->items[i];
        if (item->type == LZ_VAL_FLOAT && item->as.number != item->as.number) {
            return lz_vm_set_error(vm, LZ_ERR_RUNTIME, "%s(): cannot compare nan", name);
        }
        if (best == NULL || lz_number_order(item, best) == want) {
            best = item;
        }
    }
    *result = *best; /* a number: nothing to retain */
    return LZ_OK;
}

LzStatus lz_builtin_min(LzVM *vm, int argc, const LzValue *argv, LzValue *result) {
    (void)argc;
    return extreme(vm, "min", argv, LZ_ORDER_LESS, result);
}

LzStatus lz_builtin_max(LzVM *vm, int argc, const LzValue *argv, LzValue *result) {
    (void)argc;
    return extreme(vm, "max", argv, LZ_ORDER_GREATER, result);
}
