#include "lizard/array.h"

#include <stdlib.h>
#include <string.h>

#define LZ_VM_ARRAY_TYPES_MAX 4096

void lz_array_clear_mark(LzObject *array) {
    ((LzArray *)array)->mark = 0;
}

bool lz_array_reserve(LzHeap *heap, LzArray *array, size_t capacity) {
    if (capacity <= array->capacity) {
        return true;
    }
    if (capacity > LZ_ARRAY_MAX_LENGTH) {
        return false;
    }
    size_t extra = (capacity - array->capacity) * sizeof(LzValue);
    if (!lz_heap_charge(heap, extra)) {
        return false;
    }
    LzValue *items = realloc(array->items, capacity * sizeof(LzValue));
    if (items == NULL) {
        lz_heap_uncharge(heap, extra);
        return false;
    }
    array->items = items;
    array->capacity = capacity;
    return true;
}

LzArray *lz_array_new(LzHeap *heap, size_t capacity) {
    if (capacity > LZ_ARRAY_MAX_LENGTH) {
        return NULL;
    }
    LzArray *array = lz_heap_alloc(heap, LZ_OBJ_ARRAY, sizeof *array);
    if (array == NULL) {
        return NULL;
    }
    array->items = NULL;
    array->count = 0;
    array->capacity = 0;
    array->element = NULL;
    array->mark = 0;
    array->holders = 0;
    array->has_arrays = false;
    array->may_hold_closures = false;
    if (!lz_array_reserve(heap, array, capacity)) {
        lz_object_release(heap, &array->base);
        return NULL;
    }
    return array;
}

void lz_array_note_store(LzArray *array, const LzValue *value) {
    if (lz_value_is_array(value)) {
        LzArray *inner = lz_value_as_array(value);
        array->has_arrays = true;
        inner->holders++;
        /* The row may be given a closure later, and nothing tells the arrays that hold it:
         * only a row that cannot ever hold one (by its type) is safe to leave unflagged. */
        if (!lz_type_closure_free(inner->element)) {
            array->may_hold_closures = true;
        }
    } else if (value->type == LZ_VAL_OBJECT && value->as.object->type == LZ_OBJ_CLOSURE) {
        array->may_hold_closures = true;
    }
}

size_t lz_array_check_flags(const LzHeap *heap) {
    size_t violations = 0;
    for (const LzObject *object = heap->head; object != NULL; object = object->next) {
        if (object->type != LZ_OBJ_ARRAY) {
            continue;
        }
        const LzArray *array = (const LzArray *)object;
        if (array->may_hold_closures) {
            continue;
        }
        for (size_t i = 0; i < array->count; i++) {
            const LzValue *item = &array->items[i];
            if (item->type != LZ_VAL_OBJECT) {
                continue;
            }
            if (item->as.object->type == LZ_OBJ_CLOSURE ||
                (item->as.object->type == LZ_OBJ_ARRAY &&
                 !lz_type_closure_free(((const LzArray *)item->as.object)->element))) {
                violations++;
            }
        }
    }
    return violations;
}

static bool grow_for_one_more(LzHeap *heap, LzArray *array) {
    if (array->count < array->capacity) {
        return true;
    }
    size_t wanted = array->capacity == 0 ? 4 : array->capacity * 2;
    if (wanted > LZ_ARRAY_MAX_LENGTH) {
        wanted = LZ_ARRAY_MAX_LENGTH;
    }
    return array->count < wanted && lz_array_reserve(heap, array, wanted);
}

bool lz_array_append(LzHeap *heap, LzArray *array, const LzValue *value) {
    if (!grow_for_one_more(heap, array)) {
        return false;
    }
    lz_value_retain(value);
    array->items[array->count++] = *value;
    lz_array_note_store(array, value);
    return true;
}

bool lz_array_insert_at(LzHeap *heap, LzArray *array, size_t index, const LzValue *value) {
    if (index > array->count || !grow_for_one_more(heap, array)) {
        return false;
    }
    memmove(&array->items[index + 1], &array->items[index],
            (array->count - index) * sizeof(LzValue));
    lz_value_retain(value);
    array->items[index] = *value;
    array->count++;
    lz_array_note_store(array, value);
    return true;
}

void lz_array_remove_at(LzArray *array, size_t index, LzValue *out) {
    *out = array->items[index];
    lz_array_drop_holder(out);
    memmove(&array->items[index], &array->items[index + 1],
            (array->count - index - 1) * sizeof(LzValue));
    array->count--;
}

void lz_array_clear(LzHeap *heap, LzArray *array) {
    size_t count = array->count;
    array->count = 0;
    for (size_t i = 0; i < count; i++) {
        lz_array_drop_holder(&array->items[i]);
        lz_value_release(heap, &array->items[i]);
    }
}

bool lz_array_conform(LzArray *array, const LzType *element) {
    if (element == NULL || array->element == element) {
        return true;
    }
    if (array->element != NULL && lz_type_assignable(array->element, element) == LZ_ASSIGN_ALWAYS) {
        return true; /* already stricter than asked */
    }
    for (size_t i = 0; i < array->count; i++) {
        if (!lz_type_matches(element, &array->items[i])) {
            return false;
        }
    }
    if (array->element != NULL && lz_type_assignable(element, array->element) != LZ_ASSIGN_ALWAYS) {
        return false; /* restricted to an unrelated type already */
    }
    if ((element->kinds & LZ_KIND_ARRAY) != 0 && element->element != NULL) {
        for (size_t i = 0; i < array->count; i++) {
            if (lz_value_is_array(&array->items[i]) &&
                !lz_array_conform(lz_value_as_array(&array->items[i]), element->element)) {
                return false;
            }
        }
    }
    array->element = element;
    return true;
}

bool lz_array_admit(const LzArray *container, const LzValue *value) {
    const LzType *wanted = container->element;
    if (wanted == NULL) {
        return true;
    }
    if (lz_value_is_array(value)) {
        if ((wanted->kinds & LZ_KIND_ARRAY) == 0) {
            return false;
        }
        return lz_array_conform(lz_value_as_array(value), wanted->element);
    }
    return lz_type_matches(wanted, value);
}

int lz_array_would_cycle(LzHeap *heap, const LzValue *value, const LzArray *target,
                         uint64_t *work) {
    if (!lz_value_is_array(value)) {
        return 0;
    }
    const LzArray *root = lz_value_as_array(value);
    if (root == target) {
        return 1;
    }
    (*work)++;
    if (!root->has_arrays || target->holders == 0) {
        return 0;
    }

    uint32_t epoch = lz_heap_next_epoch(heap);
    size_t capacity = 16;
    size_t depth = 0;
    const LzArray **stack = malloc(capacity * sizeof *stack);
    if (stack == NULL) {
        return -1;
    }
    ((LzArray *)root)->mark = epoch;
    stack[depth++] = root;

    int result = 0;
    while (depth > 0 && result == 0) {
        const LzArray *array = stack[--depth];
        *work += array->count;
        for (size_t i = 0; i < array->count; i++) {
            const LzValue *item = &array->items[i];
            if (!lz_value_is_array(item)) {
                continue;
            }
            LzArray *inner = lz_value_as_array(item);
            if (inner == target) {
                result = 1;
                break;
            }
            if (inner->mark == epoch || !inner->has_arrays) {
                continue;
            }
            inner->mark = epoch;
            if (depth == capacity) {
                const LzArray **grown = realloc(stack, capacity * 2 * sizeof *stack);
                if (grown == NULL) {
                    result = -1;
                    break;
                }
                stack = grown;
                capacity *= 2;
            }
            stack[depth++] = inner;
        }
    }
    free(stack);
    return result;
}

static LzStatus store_type_error(LzVM *vm, const LzArray *array, const LzValue *value) {
    LzBuffer expected = {0};
    LzBuffer actual = {0};
    LzWriter expected_out = lz_writer_from_buffer(&expected);
    LzWriter actual_out = lz_writer_from_buffer(&actual);
    lz_type_describe(array->element, &expected_out);
    LzType shown = lz_type_of_value(value);
    if (shown.literal_count > 0) {
        shown = lz_type_widen(&shown);
    }
    lz_type_describe(&shown, &actual_out);
    LzStatus status = lz_vm_set_error(vm, LZ_ERR_TYPE, "type error: array of %s cannot hold %s",
                                      expected.data != NULL ? expected.data : "?",
                                      actual.data != NULL ? actual.data : "?");
    lz_buffer_free(&expected);
    lz_buffer_free(&actual);
    return status;
}

LzStatus lz_array_check_store(LzVM *vm, LzArray *array, const LzValue *value) {
    if (lz_value_is_array(value)) {
        uint64_t work = 0;
        int cycle = lz_array_would_cycle(&vm->heap, value, array, &work);
        LzStatus charged = lz_vm_charge_fuel(vm, lz_array_cost((size_t)work));
        if (charged != LZ_OK) {
            return charged;
        }
        if (cycle < 0) {
            return lz_vm_out_of_memory(vm);
        }
        if (cycle > 0) {
            return lz_vm_set_error(vm, LZ_ERR_RUNTIME,
                                   "cannot store an array inside itself (reference cycles are not "
                                   "supported)");
        }
        if (array->element != NULL) {
            charged = lz_vm_charge_fuel(vm, lz_array_cost(lz_value_as_array(value)->count));
            if (charged != LZ_OK) {
                return charged;
            }
        }
    }
    if (!lz_array_admit(array, value)) {
        return store_type_error(vm, array, value);
    }
    return LZ_OK;
}

static LzStatus too_large(LzVM *vm) {
    return lz_vm_set_error(vm, LZ_ERR_RUNTIME, "array too large (limit %zu items)",
                           (size_t)LZ_ARRAY_MAX_LENGTH);
}

LzStatus lz_array_push(LzVM *vm, LzArray *array, const LzValue *value) {
    LzStatus status = lz_array_check_store(vm, array, value);
    if (status != LZ_OK) {
        return status;
    }
    if (array->count >= LZ_ARRAY_MAX_LENGTH) {
        return too_large(vm);
    }
    return lz_array_append(&vm->heap, array, value) ? LZ_OK : lz_vm_out_of_memory(vm);
}

LzStatus lz_array_resolve_index(LzVM *vm, const LzValue *index, size_t count, size_t *out) {
    if (index->type != LZ_VAL_INT) {
        return lz_vm_set_error(vm, LZ_ERR_RUNTIME, "array index must be an int, got %s",
                               lz_kind_name(lz_kind_of(index)));
    }
    int64_t position = index->as.integer;
    int64_t resolved = position < 0 ? position + (int64_t)count : position;
    if (resolved < 0 || resolved >= (int64_t)count) {
        return lz_vm_set_error(vm, LZ_ERR_RUNTIME,
                               "index %lld out of range for array of length %zu",
                               (long long)position, count);
    }
    *out = (size_t)resolved;
    return LZ_OK;
}

LzStatus lz_array_set(LzVM *vm, LzArray *array, const LzValue *index, const LzValue *value) {
    size_t position = 0;
    LzStatus status = lz_array_resolve_index(vm, index, array->count, &position);
    if (status != LZ_OK) {
        return status;
    }
    status = lz_array_check_store(vm, array, value);
    if (status != LZ_OK) {
        return status;
    }
    LzValue old = array->items[position];
    lz_array_drop_holder(&old);
    lz_value_retain(value);
    array->items[position] = *value;
    lz_array_note_store(array, value);
    lz_value_release(&vm->heap, &old);
    return LZ_OK;
}

static LzStatus bound(LzVM *vm, const LzValue *value, int64_t count, int64_t fallback,
                      int64_t *out) {
    if (value->type == LZ_VAL_NIL) {
        *out = fallback;
        return LZ_OK;
    }
    if (value->type != LZ_VAL_INT) {
        return lz_vm_set_error(vm, LZ_ERR_RUNTIME, "slice bounds must be ints, got %s",
                               lz_kind_name(lz_kind_of(value)));
    }
    int64_t position = value->as.integer;
    if (position < 0) {
        position = position < -count ? 0 : position + count;
    } else if (position > count) {
        position = count;
    }
    *out = position;
    return LZ_OK;
}

static LzStatus copy_range(LzVM *vm, const LzArray *array, size_t from, size_t to,
                           LzValue *result) {
    LzStatus charged = lz_vm_charge_fuel(vm, lz_array_cost(to - from));
    if (charged != LZ_OK) {
        return charged;
    }
    LzArray *copy = lz_array_new(&vm->heap, to - from);
    if (copy == NULL) {
        return lz_vm_out_of_memory(vm);
    }
    for (size_t i = from; i < to; i++) {
        lz_value_retain(&array->items[i]);
        lz_array_note_store(copy, &array->items[i]);
        copy->items[copy->count++] = array->items[i];
    }
    copy->element = array->element;
    copy->has_arrays = array->has_arrays;
    *result = lz_value_object(&copy->base);
    return LZ_OK;
}

LzStatus lz_array_slice(LzVM *vm, const LzArray *array, const LzValue *start, const LzValue *end,
                        LzValue *result) {
    int64_t count = (int64_t)array->count;
    int64_t from = 0;
    int64_t to = 0;
    LzStatus status = bound(vm, start, count, 0, &from);
    if (status == LZ_OK) {
        status = bound(vm, end, count, count, &to);
    }
    if (status != LZ_OK) {
        return status;
    }
    if (to < from) {
        to = from;
    }
    return copy_range(vm, array, (size_t)from, (size_t)to, result);
}

LzStatus lz_array_copy(LzVM *vm, const LzArray *array, LzValue *result) {
    return copy_range(vm, array, 0, array->count, result);
}

LzStatus lz_array_concat(LzVM *vm, const LzArray *a, const LzArray *b, LzValue *result) {
    if (a->count > LZ_ARRAY_MAX_LENGTH - b->count) {
        return too_large(vm);
    }
    LzStatus charged = lz_vm_charge_fuel(vm, lz_array_cost(a->count + b->count));
    if (charged != LZ_OK) {
        return charged;
    }
    LzArray *joined = lz_array_new(&vm->heap, a->count + b->count);
    if (joined == NULL) {
        return lz_vm_out_of_memory(vm);
    }
    for (size_t i = 0; i < a->count; i++) {
        lz_value_retain(&a->items[i]);
        lz_array_note_store(joined, &a->items[i]);
        joined->items[joined->count++] = a->items[i];
    }
    for (size_t i = 0; i < b->count; i++) {
        lz_value_retain(&b->items[i]);
        lz_array_note_store(joined, &b->items[i]);
        joined->items[joined->count++] = b->items[i];
    }
    /* Items of both sides fit the tag only when the tags agree. */
    joined->element = a->element == b->element ? a->element : NULL;
    joined->has_arrays = a->has_arrays || b->has_arrays;
    *result = lz_value_object(&joined->base);
    return LZ_OK;
}

LzStatus lz_vm_array_of(LzVM *vm, const LzType *element, LzType *out) {
    LzType array = lz_type_of_kinds(LZ_KIND_ARRAY);
    bool is_any = element == NULL || (element->kinds == LZ_KINDS_ANY && element->element == NULL);
    if (!is_any) {
        const LzType *found = NULL;
        for (size_t i = 0; i < vm->array_type_count && found == NULL; i++) {
            if (lz_type_equal(vm->array_types[i], element)) {
                found = vm->array_types[i];
            }
        }
        if (found == NULL) {
            if (vm->array_type_count >= LZ_VM_ARRAY_TYPES_MAX) {
                return lz_vm_set_error(vm, LZ_ERR_COMPILE, "too many distinct array types");
            }
            if (vm->array_type_count == vm->array_type_capacity) {
                size_t capacity = vm->array_type_capacity != 0 ? vm->array_type_capacity * 2 : 16;
                LzType **grown = realloc(vm->array_types, capacity * sizeof *grown);
                if (grown == NULL) {
                    return lz_vm_set_error(vm, LZ_ERR_MEMORY, "out of memory");
                }
                vm->array_types = grown;
                vm->array_type_capacity = capacity;
            }
            LzType *copy = malloc(sizeof *copy);
            if (copy == NULL) {
                return lz_vm_set_error(vm, LZ_ERR_MEMORY, "out of memory");
            }
            *copy = *element;
            vm->array_types[vm->array_type_count++] = copy;
            found = copy;
        }
        array.element = found;
    }
    *out = array;
    return LZ_OK;
}

LzStatus lz_vm_values_equal(LzVM *vm, const LzValue *a, const LzValue *b, bool *same) {
    if (a->type == LZ_VAL_OBJECT && b->type == LZ_VAL_OBJECT && a->as.object != b->as.object &&
        a->as.object->type == LZ_OBJ_STRING && b->as.object->type == LZ_OBJ_STRING) {
        const LzString *x = (const LzString *)a->as.object;
        const LzString *y = (const LzString *)b->as.object;
        if (x->length == y->length) {
            LzStatus charged = lz_vm_charge_fuel(vm, x->length / 64);
            if (charged != LZ_OK) {
                return charged;
            }
        }
    }

    uint64_t work = UINT64_MAX;
    if (vm->fuel != LZ_FUEL_UNLIMITED) {
        work = vm->fuel > (UINT64_MAX - 7) / 8 ? UINT64_MAX - 1 : vm->fuel * 8 + 7;
    }
    *same = lz_value_equal_budget(a, b, &work);
    if (work == UINT64_MAX) {
        vm->fuel = 0;
        return lz_vm_set_error(vm, LZ_ERR_FUEL, "instruction limit exceeded");
    }
    return lz_vm_charge_fuel(vm, lz_array_cost((size_t)work));
}
