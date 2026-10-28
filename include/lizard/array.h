#ifndef LIZARD_ARRAY_H
#define LIZARD_ARRAY_H

#include "lizard/type.h"
#include "lizard/vm.h"

/* Hard cap on the number of items, so one script cannot ask for gigabytes. */
#define LZ_ARRAY_MAX_LENGTH ((size_t)1 << 24)

static inline bool lz_value_is_array(const LzValue *value) {
    return value->type == LZ_VAL_OBJECT && value->as.object->type == LZ_OBJ_ARRAY;
}

static inline LzArray *lz_value_as_array(const LzValue *value) {
    return (LzArray *)value->as.object;
}

/* An array item that is itself an array records who holds it; see lz_array_would_cycle. */
static inline void lz_array_drop_holder(const LzValue *item) {
    if (lz_value_is_array(item)) {
        lz_value_as_array(item)->holders--;
    }
}

/* Used by the heap when a stamp counter wraps. */
void lz_array_clear_mark(LzObject *array);

/*
 * New empty array with room for capacity items. The item buffer counts
 * against the heap limit. NULL when out of memory, over the limit, or
 * capacity > LZ_ARRAY_MAX_LENGTH.
 */
LzArray *lz_array_new(LzHeap *heap, size_t capacity);

/* Makes room for at least capacity items. Same failure rules as lz_array_new. */
bool lz_array_reserve(LzHeap *heap, LzArray *array, size_t capacity);

/* Appends a value (retained). No type or cycle checks: see lz_array_push. */
bool lz_array_append(LzHeap *heap, LzArray *array, const LzValue *value);

/* Inserts at index <= count, shifting later items. No checks. */
bool lz_array_insert_at(LzHeap *heap, LzArray *array, size_t index, const LzValue *value);

/* Removes the item at index; *out receives the reference the array held. */
void lz_array_remove_at(LzArray *array, size_t index, LzValue *out);

/* Releases every item. The buffer is kept. */
void lz_array_clear(LzHeap *heap, LzArray *array);

/*
 * Can `value` be stored in this array? Checks the array's element type, and
 * gives an array value the element type the slot expects, so a `int[][]`
 * keeps its rows typed.
 */
bool lz_array_admit(const LzArray *container, const LzValue *value);

/*
 * Makes `array` obey `element` (NULL = any): every item must match it, and
 * afterwards so must every later store. False when an item does not match, or
 * the array is already restricted to an unrelated type.
 */
bool lz_array_conform(LzArray *array, const LzType *element);

/*
 * Would storing `value` in `target` create a reference cycle? 1 yes, 0 no,
 * -1 out of memory. *work counts the arrays visited. An array that no other
 * array holds cannot be inside `value`, so the common case costs nothing.
 */
int lz_array_would_cycle(LzHeap *heap, const LzValue *value, const LzArray *target, uint64_t *work);

/* Fuel for a pass over n items: one unit per 8. */
static inline uint64_t lz_array_cost(size_t n) {
    return (uint64_t)n / 8;
}

/*
 * The checked store used by every operation that puts a value into an array:
 * element type, cycle check, fuel. On success the array has no new item yet;
 * the caller appends or assigns, then calls lz_array_note_store.
 */
LzStatus lz_array_check_store(LzVM *vm, LzArray *array, const LzValue *value);
void lz_array_note_store(LzArray *array, const LzValue *value);

/* Checked append: lz_array_check_store plus lz_array_append. */
LzStatus lz_array_push(LzVM *vm, LzArray *array, const LzValue *value);

/*
 * Resolves an index (negative counts from the end) against count. Fails with
 * a runtime error when the value is not an int or is out of range.
 */
LzStatus lz_array_resolve_index(LzVM *vm, const LzValue *index, size_t count, size_t *out);

/* container[start:end]; either bound may be nil. *result is a new reference. */
LzStatus lz_array_slice(LzVM *vm, const LzArray *array, const LzValue *start, const LzValue *end,
                        LzValue *result);

/* a + b. *result is a new reference. */
LzStatus lz_array_concat(LzVM *vm, const LzArray *a, const LzArray *b, LzValue *result);

/* container[index] = value, fully checked. */
LzStatus lz_array_set(LzVM *vm, LzArray *array, const LzValue *index, const LzValue *value);

/* Shallow copy keeping the element type. */
LzStatus lz_array_copy(LzVM *vm, const LzArray *array, LzValue *result);

/*
 * a == b with fuel accounting: two different equal-length strings cost one
 * unit per 64 bytes, arrays one per 8 item pairs, and a walk that would use
 * more fuel than is left fails with LZ_ERR_FUEL.
 */
LzStatus lz_vm_values_equal(LzVM *vm, const LzValue *a, const LzValue *b, bool *same);

/* Builds the type `element[]`. The element type is interned in the VM. */
LzStatus lz_vm_array_of(LzVM *vm, const LzType *element, LzType *out);

#endif
