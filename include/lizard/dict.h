#ifndef LIZARD_DICT_H
#define LIZARD_DICT_H

#include "lizard/type.h"
#include "lizard/vm.h"

/* Hard cap on the number of entries, so one script can't ask for gigabytes. */
#define LZ_DICT_MAX_LENGTH ((size_t)1 << 22)

/*
 * One entry in insertion order. `hash` is the key's hash, so a resize or a lookup never hashes
 * the same key twice.
 */
typedef struct LzDictEntry {
    LzValue key;
    LzValue value;
    uint64_t hash;
} LzDictEntry;

/*
 * An ordered map. `entries` is dense in insertion order, which is the order the dict prints,
 * compares and iterates in. `slots` is an open-addressing index into it, where 0 is empty and
 * anything else is entry index + 1. Overwriting a key keeps its place, and removing one shifts the
 * entries after it down, so a key added again lands at the end. Keys are strings, ints or bools
 * only.
 */
typedef struct LzDict {
    LzObject base;
    LzDictEntry *entries;
    size_t count;
    size_t capacity;
    uint32_t *slots;
    size_t slot_capacity; /* a power of two, at least twice the count */
    uint64_t seed;        /* per-dict hash seed, so one script can't flood the table */
    uint32_t mark;        /* graph-walk stamp, see lz_heap_next_epoch */
    size_t holders;       /* how many containers currently hold this dict */
    const struct LzType *key_type;   /* NULL means any key kind */
    const struct LzType *value_type; /* NULL means any value */
} LzDict;

static inline bool lz_value_is_dict(const LzValue *value) {
    return value->type == LZ_VAL_OBJECT && value->as.object->type == LZ_OBJ_DICT;
}

static inline LzDict *lz_value_as_dict(const LzValue *value) {
    return (LzDict *)value->as.object;
}

/* An array, a dict or an instance, which are the values reference cycles can run through. */
static inline bool lz_value_is_container(const LzValue *value) {
    return value->type == LZ_VAL_OBJECT &&
           (value->as.object->type == LZ_OBJ_ARRAY || value->as.object->type == LZ_OBJ_DICT ||
            value->as.object->type == LZ_OBJ_INSTANCE);
}

/* Used by the heap when a stamp counter wraps. */
void lz_dict_clear_mark(LzObject *dict);

/* New dict with room for capacity entries. NULL when out of memory or over the limit. */
LzDict *lz_dict_new(LzHeap *heap, size_t capacity);

/* The value stored under key, or NULL. The entry stays owned by the dict. */
LzValue *lz_dict_get(const LzDict *dict, const LzValue *key);

/* The entry index of key, or SIZE_MAX when it is absent. */
size_t lz_dict_index_of(const LzDict *dict, const LzValue *key);

/* d[key] = value. Adds, or overwrites in place. Checked for key kind, type, cycle and fuel. */
LzStatus lz_dict_set(LzVM *vm, LzDict *dict, const LzValue *key, const LzValue *value);

/* Removes key and *out receives the reference the dict held. Fails when it's absent. */
LzStatus lz_dict_remove(LzVM *vm, LzDict *dict, const LzValue *key, LzValue *out);

/* Releases every entry. The buffers are kept. */
void lz_dict_clear(LzHeap *heap, LzDict *dict);

/*
 * Makes `dict` obey (keys, values), where NULL means any. Every entry must fit, and so must
 * every later store. False when an entry doesn't, or the dict is already restricted to unrelated
 * types.
 */
bool lz_dict_conform(LzDict *dict, const LzType *keys, const LzType *values);

/* Would this value fit the dict's value type? */
bool lz_dict_value_fits(const LzDict *dict, const LzValue *value);

/* Shallow copy, keeping the key and value types. *result is a new reference. */
LzStatus lz_dict_copy(LzVM *vm, const LzDict *dict, LzValue *result);

/* a merged with b. b wins on a shared key, the result is new, and the order is a's and then b's. */
LzStatus lz_dict_merge(LzVM *vm, const LzDict *a, const LzDict *b, LzValue *result);

/* New arrays of the keys in order, the values in order, and a [key, value] pair per entry. */
LzStatus lz_dict_keys(LzVM *vm, const LzDict *dict, LzValue *result);
LzStatus lz_dict_values(LzVM *vm, const LzDict *dict, LzValue *result);
LzStatus lz_dict_items(LzVM *vm, const LzDict *dict, LzValue *result);

/* The checked store used by every operation that puts a value into a dict. */
LzStatus lz_dict_check_store(LzVM *vm, LzDict *dict, const LzValue *value);

/* Fails with "a <kind> cannot be a dict key" unless key is a string, an int or a bool. */
LzStatus lz_dict_own_key(LzVM *vm, const LzValue *key);

/* Does the key fit the dict's key type? Also rejects the kinds that are never keys. */
bool lz_dict_admit_key(const LzDict *dict, const LzValue *key);

/* `key "x" not found`, with `after` appended, and a hint from the dict's string keys. */
LzStatus lz_vm_dict_key_error(LzVM *vm, const LzDict *dict, const LzValue *key, const char *after);

/* The key as a message shows it: strings quoted, numbers and bools bare. */
void lz_dict_write_key(const LzValue *key, const LzWriter *out);

/* Fuel for a pass over n entries, one unit per 8. */
static inline uint64_t lz_dict_cost(size_t n) {
    return (uint64_t)n / 8;
}

#endif
