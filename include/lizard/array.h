#ifndef LIZARD_ARRAY_H
#define LIZARD_ARRAY_H

#include "lizard/type.h"
#include "lizard/vm.h"

/* Hard cap on the number of items, so one script can't ask for gigabytes. */
#define LZ_ARRAY_MAX_LENGTH ((size_t)1 << 24)

static inline bool lz_value_is_array(const LzValue *value) {
    return value->type == LZ_VAL_OBJECT && value->as.object->type == LZ_OBJ_ARRAY;
}

static inline LzArray *lz_value_as_array(const LzValue *value) {
    return (LzArray *)value->as.object;
}

/*
 * A container held by another one records who holds it, see lz_containers_would_cycle. Only
 * arrays count their holders, so only dropping one of them is cheap here.
 */
/* Defined with the instances in class.c. One container fewer holds this instance. */
void lz_instance_drop_holder(const LzValue *item);

static inline void lz_array_drop_holder(const LzValue *item) {
    if (lz_value_is_array(item)) {
        lz_value_as_array(item)->holders--;
    } else if (item->type == LZ_VAL_OBJECT && item->as.object->type == LZ_OBJ_INSTANCE) {
        lz_instance_drop_holder(item);
    }
}

/* The holder bookkeeping of any container, which is an array item or a dict value. */
void lz_note_container_store(LzHeap *heap, const LzValue *value);
void lz_drop_container_holder(const LzValue *value);

/*
 * A container, or an error, which are the values the cycle check has to look into. An error
 * carries data.
 */
static inline bool lz_value_may_hold_container(const LzValue *value) {
    return value->type == LZ_VAL_OBJECT &&
           (value->as.object->type == LZ_OBJ_ARRAY || value->as.object->type == LZ_OBJ_DICT ||
            value->as.object->type == LZ_OBJ_ERROR || value->as.object->type == LZ_OBJ_INSTANCE);
}

/*
 * Would storing `value` in the container `target` create a reference cycle? 1 means yes, 0 no,
 * and -1 out of memory. It walks arrays, dicts and errors (cause and data), and *work counts the
 * items visited.
 */
int lz_containers_would_cycle(LzHeap *heap, const LzValue *value, const LzObject *target,
                              uint64_t *work);

/* Used by the heap when a stamp counter wraps. */
void lz_array_clear_mark(LzObject *array);

/*
 * New empty array with room for capacity items. The item buffer counts against the heap limit.
 * NULL when out of memory, over the limit, or capacity > LZ_ARRAY_MAX_LENGTH.
 */
LzArray *lz_array_new(LzHeap *heap, size_t capacity);

/* Makes room for at least capacity items. Same failure rules as lz_array_new. */
bool lz_array_reserve(LzHeap *heap, LzArray *array, size_t capacity);

/* Appends a value, retained. No type or cycle checks, see lz_array_push. */
bool lz_array_append(LzHeap *heap, LzArray *array, const LzValue *value);

/* Inserts at index <= count and shifts later items. No checks. */
bool lz_array_insert_at(LzHeap *heap, LzArray *array, size_t index, const LzValue *value);

/* Removes the item at index and *out receives the reference the array held. */
void lz_array_remove_at(LzArray *array, size_t index, LzValue *out);

/* Releases every item and keeps the buffer. */
void lz_array_clear(LzHeap *heap, LzArray *array);

/*
 * Can `value` be stored in this array? Checks the array's element type, and gives an array value
 * the element type the slot expects, so an `int[][]` keeps its rows typed.
 */
bool lz_array_admit(const LzArray *container, const LzValue *value);

/*
 * Makes `array` obey `element`, where NULL means any. Every item must match it, and so must every
 * later store. False when an item doesn't match, or the array is already restricted to an
 * unrelated type.
 */
bool lz_array_conform(LzArray *array, const LzType *element);

/* Fuel for a pass over n items, one unit per 8. */
static inline uint64_t lz_array_cost(size_t n) {
    return (uint64_t)n / 8;
}

/*
 * The checked store used by every operation that puts a value into an array. It checks element
 * type, cycle and fuel. On success the array has no new item yet, so the caller appends or
 * assigns and then calls lz_array_note_store.
 */
LzStatus lz_array_check_store(LzVM *vm, LzArray *array, const LzValue *value);
void lz_array_note_store(LzArray *array, const LzValue *value);

/* Checked append, which is lz_array_check_store plus lz_array_append. */
LzStatus lz_array_push(LzVM *vm, LzArray *array, const LzValue *value);

/*
 * Resolves an index against count, where a negative one counts from the end. Fails with a runtime
 * error when the value is not an int or is out of range.
 */
LzStatus lz_array_resolve_index(LzVM *vm, const LzValue *index, size_t count, size_t *out);

/* container[start:end], where either bound may be nil. *result is a new reference. */
LzStatus lz_array_slice(LzVM *vm, const LzArray *array, const LzValue *start, const LzValue *end,
                        LzValue *result);

/* a + b. *result is a new reference. */
LzStatus lz_array_concat(LzVM *vm, const LzArray *a, const LzArray *b, LzValue *result);

/* container[index] = value, fully checked. */
LzStatus lz_array_set(LzVM *vm, LzArray *array, const LzValue *index, const LzValue *value);

/*
 * What lz_array_set does once the position is known and lz_array_check_store has passed: the new
 * value takes the slot and the old one is released. Shared with the interpreter's inline
 * SET_INDEX, so the holder bookkeeping has one definition.
 */
static inline void lz_array_store_at(LzHeap *heap, LzArray *array, size_t position,
                                     const LzValue *value) {
    LzValue old = array->items[position];
    lz_array_drop_holder(&old);
    lz_value_retain(value);
    array->items[position] = *value;
    lz_array_note_store(array, value);
    lz_value_release(heap, &old);
}

/* Shallow copy keeping the element type. */
LzStatus lz_array_copy(LzVM *vm, const LzArray *array, LzValue *result);

/*
 * a == b with fuel accounting. Two different equal-length strings cost one unit per 64 bytes,
 * arrays one per 8 item pairs, and a walk that would use more fuel than is left fails with
 * LZ_ERR_FUEL.
 */
LzStatus lz_vm_values_equal(LzVM *vm, const LzValue *a, const LzValue *b, bool *same);

/* Builds the type `element[]`. The element type is interned in the VM. */
LzStatus lz_vm_array_of(LzVM *vm, const LzType *element, LzType *out);

/*
 * An interned copy of type, so two equal types are the same pointer. Used for array and dict
 * types.
 */
LzStatus lz_vm_intern_type(LzVM *vm, const LzType *type, LzType **out);

/*
 * Builds the type `fn(params) -> result`. `params` has param_count entries, and a NULL entry, or a
 * type that is `any`, means any. A NULL `result` is a result nobody cares about, and a NULL `rest`
 * means any and only counts with has_rest. The types are interned in the VM, and so is the
 * signature, so two calls with equal arguments give types with the same `fn` pointer.
 */
LzStatus lz_vm_fn_of(LzVM *vm, size_t param_count, size_t required, bool has_rest,
                     const LzType *const *params, const LzType *result, const LzType *rest,
                     LzType *out);

#endif
