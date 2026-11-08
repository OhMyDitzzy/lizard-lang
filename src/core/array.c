#include "lizard/array.h"
#include "lizard/class.h"

#include <stdlib.h>
#include <string.h>

#include "lizard/dict.h"

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
    if (!lz_array_reserve(heap, array, capacity)) {
        lz_object_release(heap, &array->base);
        return NULL;
    }
    return array;
}

void lz_note_container_store(LzHeap *heap, const LzValue *value) {
    (void)heap;
    if (lz_value_is_array(value)) {
        lz_value_as_array(value)->holders++;
    } else if (lz_value_is_dict(value)) {
        lz_value_as_dict(value)->holders++;
    } else if (lz_value_is_instance(value)) {
        lz_value_as_instance(value)->holders++;
    }
}

void lz_drop_container_holder(const LzValue *value) {
    if (lz_value_is_array(value)) {
        lz_value_as_array(value)->holders--;
    } else if (lz_value_is_dict(value)) {
        lz_value_as_dict(value)->holders--;
    } else if (lz_value_is_instance(value)) {
        lz_value_as_instance(value)->holders--;
    }
}

void lz_array_note_store(LzArray *array, const LzValue *value) {
    if (lz_value_is_array(value)) {
        array->has_arrays = true;
        lz_value_as_array(value)->holders++;
    } else if (lz_value_is_dict(value)) {
        array->has_arrays = true; /* the walk has to visit it too */
        lz_value_as_dict(value)->holders++;
    } else if (lz_value_is_instance(value)) {
        array->has_arrays = true; /* the walk has to visit its fields */
        lz_value_as_instance(value)->holders++;
    } else if (lz_value_is_error(value)) {
        array->has_arrays = true; /* an error leads to its data, which the walk has to visit */
    }
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

/* The containers a value leads to, one at a time: an array's items, a dict's values, an instance's fields. */
static bool container_is_array(const LzObject *object) {
    return object->type == LZ_OBJ_ARRAY;
}

static bool container_may_open(const LzObject *object) {
    if (container_is_array(object)) {
        return ((const LzArray *)object)->has_arrays;
    }
    if (object->type == LZ_OBJ_INSTANCE) {
        return ((const LzInstance *)object)->cls->field_count != 0;
    }
    return ((const LzDict *)object)->count != 0;
}

static size_t container_items(const LzObject *object) {
    if (object->type == LZ_OBJ_INSTANCE) {
        return ((const LzInstance *)object)->cls->field_count;
    }
    return container_is_array(object) ? ((const LzArray *)object)->count
                                      : ((const LzDict *)object)->count;
}

static const LzValue *container_item_at(const LzObject *object, size_t index) {
    if (object->type == LZ_OBJ_INSTANCE) {
        return &((const LzInstance *)object)->fields[index];
    }
    return container_is_array(object) ? &((const LzArray *)object)->items[index]
                                      : &((const LzDict *)object)->entries[index].value;
}

static uint32_t *container_mark(LzObject *object) {
    if (object->type == LZ_OBJ_INSTANCE) {
        return &((LzInstance *)object)->mark;
    }
    return container_is_array(object) ? &((LzArray *)object)->mark : &((LzDict *)object)->mark;
}

static size_t container_holders(const LzObject *object) {
    if (object->type == LZ_OBJ_INSTANCE) {
        return ((const LzInstance *)object)->holders;
    }
    return object->type == LZ_OBJ_ARRAY ? ((const LzArray *)object)->holders
                                        : ((const LzDict *)object)->holders;
}

/* The state of one walk: what it looks for, the stamp of its visits, the containers still to open. */
typedef struct CycleWalk {
    const LzObject *target;
    uint32_t epoch;
    const LzObject **stack;
    size_t depth;
    size_t capacity;
    int result; /* 0 so far, 1 found, -1 out of memory */
} CycleWalk;

static bool walk_push(CycleWalk *walk, const LzObject *container) {
    if (walk->depth == walk->capacity) {
        const LzObject **grown = realloc(walk->stack, walk->capacity * 2 * sizeof *grown);
        if (grown == NULL) {
            walk->result = -1;
            return false;
        }
        walk->stack = grown;
        walk->capacity *= 2;
    }
    walk->stack[walk->depth++] = container;
    return true;
}

/*
 * Looks at one value met on the way: a container is the target, or is opened later (once per
 * walk); an error leads to its cause, its data and its payload, a chain of errors one after the other.
 * The causes of an error were made before it, so that chain ends; the containers it leads to
 * are stamped, so none is opened twice.
 */
static void walk_object(CycleWalk *walk, LzObject *inner) {
    if (inner == walk->target) {
        walk->result = 1;
        return;
    }
    uint32_t *mark = container_mark(inner);
    if (*mark == walk->epoch || !container_may_open(inner)) {
        return;
    }
    *mark = walk->epoch;
    (void)walk_push(walk, inner);
}

static void walk_value(CycleWalk *walk, const LzValue *value, uint64_t *work) {
    while (lz_value_is_error(value)) {
        const LzError *error = (const LzError *)value->as.object;
        (*work)++;
        walk_value(walk, &error->data, work);
        if (walk->result != 0) {
            return;
        }
        if (error->payload != NULL) {
            /* the fields of an error of a class live in its payload, a container like any instance */
            walk_object(walk, &error->payload->base);
            if (walk->result != 0) {
                return;
            }
        }
        value = &error->cause;
    }
    if (!lz_value_is_container(value)) {
        return;
    }
    walk_object(walk, value->as.object);
}

int lz_containers_would_cycle(LzHeap *heap, const LzValue *value, const LzObject *target,
                              uint64_t *work) {
    if (!lz_value_may_hold_container(value)) {
        return 0;
    }
    if (lz_value_is_container(value)) {
        LzObject *root = value->as.object;
        if (root == target) {
            return 1;
        }
        (*work)++;
        /*
         * A container that nothing holds cannot be inside `value` (reaching one would need a
         * holder), so the common store of a freshly built structure costs nothing. An error
         * that carries a container as its data counts as a holder of it.
         */
        if (!container_may_open(root) || target == NULL) {
            return 0;
        }
    } else if (target == NULL) {
        return 0;
    }
    if (container_holders(target) == 0) {
        return 0;
    }

    CycleWalk walk = {target, lz_heap_next_epoch(heap), NULL, 0, 16, 0};
    walk.stack = malloc(walk.capacity * sizeof *walk.stack);
    if (walk.stack == NULL) {
        return -1;
    }
    if (lz_value_is_container(value)) {
        *container_mark(value->as.object) = walk.epoch;
        (void)walk_push(&walk, value->as.object);
    } else {
        walk_value(&walk, value, work); /* an error: its causes and data are the roots */
    }

    while (walk.depth > 0 && walk.result == 0) {
        LzObject *container = (LzObject *)walk.stack[--walk.depth];
        size_t count = container_items(container);
        *work += count;
        for (size_t i = 0; i < count && walk.result == 0; i++) {
            walk_value(&walk, container_item_at(container, i), work);
        }
    }
    free(walk.stack);
    return walk.result;
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
    if (lz_value_may_hold_container(value)) {
        uint64_t work = 0;
        int cycle = lz_containers_would_cycle(&vm->heap, value, &array->base, &work);
        LzStatus charged = lz_vm_charge_fuel(vm, lz_array_cost((size_t)work));
        if (charged != LZ_OK) {
            return charged;
        }
        if (cycle < 0) {
            return lz_vm_out_of_memory(vm);
        }
        if (cycle > 0) {
            return lz_vm_cycle_error(vm, NULL, NULL, 'a');
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
                               lz_value_kind_name(index));
    }
    int64_t position = index->as.integer;
    int64_t resolved = position < 0 ? position + (int64_t)count : position;
    if (resolved < 0 || resolved >= (int64_t)count) {
        LzStatus status = lz_vm_set_error(vm, LZ_ERR_RUNTIME,
                                          "index %lld out of range for array of length %zu",
                                          (long long)position, count);
        if (count == 0) {
            lz_vm_set_help(vm, "the array is empty; check len(a) before reading an item");
        } else {
            lz_vm_set_help(vm, "valid indexes are 0..%zu (or -1..-%zu from the end)", count - 1, count);
        }
        return status;
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
                               lz_value_kind_name(value));
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

/* The VM's copy of `type`, shared so that two equal types are the same pointer. */
LzStatus lz_vm_intern_type(LzVM *vm, const LzType *type, LzType **out) {
    for (size_t i = 0; i < vm->array_type_count; i++) {
        if (lz_type_equal(vm->array_types[i], type)) {
            *out = vm->array_types[i];
            return LZ_OK;
        }
    }
    if (vm->array_type_count >= LZ_VM_ARRAY_TYPES_MAX) {
        return lz_vm_set_error(vm, LZ_ERR_COMPILE, "too many distinct types");
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
    *copy = *type;
    vm->array_types[vm->array_type_count++] = copy;
    *out = copy;
    return LZ_OK;
}

LzStatus lz_vm_array_of(LzVM *vm, const LzType *element, LzType *out) {
    LzType array = lz_type_of_kinds(LZ_KIND_ARRAY);
    bool is_any = element == NULL || (element->kinds == LZ_KINDS_ANY && element->element == NULL);
    if (!is_any) {
        LzType *found = NULL;
        LzStatus status = lz_vm_intern_type(vm, element, &found);
        if (status != LZ_OK) {
            return status;
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
    LzVM *saved_scope = NULL;
    lz_class_equal_scope_begin(vm, &saved_scope);
    *same = lz_value_equal_budget(a, b, &work);
    LzStatus operator_status = lz_class_equal_scope_end(saved_scope);
    if (operator_status != LZ_OK) {
        *same = false;
        return operator_status;
    }
    if (work == UINT64_MAX) {
        vm->fuel = 0;
        return lz_vm_set_error(vm, LZ_ERR_FUEL, "instruction limit exceeded");
    }
    return lz_vm_charge_fuel(vm, lz_array_cost((size_t)work));
}
