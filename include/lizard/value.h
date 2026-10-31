#ifndef LIZARD_VALUE_H
#define LIZARD_VALUE_H

#include "lizard/memory.h"
#include "lizard/writer.h"

typedef enum LzValueType {
    LZ_VAL_NIL,
    LZ_VAL_BOOL,
    LZ_VAL_INT,
    LZ_VAL_FLOAT,
    LZ_VAL_OBJECT
} LzValueType;

/*
 * Scalars live inline in the value. Everything else is a pointer to a heap
 * object owned through the refcount in LzObject.
 */
typedef struct LzValue {
    LzValueType type;
    union {
        bool boolean;
        int64_t integer;
        double number;
        LzObject *object;
    } as;
} LzValue;

typedef struct LzString {
    LzObject base;
    size_t length;
    uint32_t hash;
    char data[];
} LzString;

struct LzType;

/*
 * A growable list. `element` is the type every item must have, or NULL for any
 * (an unannotated array). It is set when the array is stored under an annotated
 * type such as `int[]`, and every later store is checked against it. Types
 * pointed at here are owned by the VM (lz_vm_array_of).
 */
typedef struct LzArray {
    LzObject base;
    LzValue *items;
    size_t count;
    size_t capacity;
    const struct LzType *element;
    size_t holders;   /* how many array items currently hold this array */
    uint32_t mark;    /* graph-walk stamp, see lz_heap_next_epoch */
    bool has_arrays;  /* some item may be an array (never reset); lets walks skip flat arrays */
} LzArray;

typedef enum LzOrder {
    LZ_ORDER_LESS = -1,
    LZ_ORDER_EQUAL = 0,
    LZ_ORDER_GREATER = 1,
    LZ_ORDER_UNORDERED = 2 /* at least one side is NaN */
} LzOrder;

LzValue lz_value_nil(void);
LzValue lz_value_bool(bool value);
LzValue lz_value_int(int64_t value);
LzValue lz_value_float(double value);
LzValue lz_value_object(LzObject *object);

uint32_t lz_hash_bytes(const char *chars, size_t length);

LzString *lz_string_new(LzHeap *heap, const char *chars, size_t length);

/* New string holding a followed by b. NULL when out of memory or the length overflows. */
LzString *lz_string_concat(LzHeap *heap, const LzString *a, const LzString *b);

void lz_value_retain(const LzValue *value);
void lz_value_release(LzHeap *heap, const LzValue *value);

/* Only nil and false are falsy. */
bool lz_value_is_truthy(const LzValue *value);

/*
 * Exact comparison of two numbers (int or float), including int against float
 * without losing precision above 2^53. Both values must be numbers.
 */
LzOrder lz_number_order(const LzValue *left, const LzValue *right);

/*
 * No implicit conversion between types, except that ints and floats compare
 * by numeric value. Strings compare by content; other objects by identity.
 */
bool lz_value_equal(const LzValue *left, const LzValue *right);

/*
 * Same result as lz_value_equal, but arrays are compared by content with a
 * work budget (one unit per item pair). *work is the budget on entry and the
 * work done on exit; when the budget runs out the walk stops, returns false
 * and sets *work to UINT64_MAX so the caller can report it.
 */
bool lz_value_equal_budget(const LzValue *left, const LzValue *right, uint64_t *work);

/*
 * Writes the user-visible text form of a value. Strings inside an array are
 * quoted. Returns false on an output error (or out of memory for a deep array).
 */
bool lz_value_write(const LzValue *value, const LzWriter *writer);

/* Like lz_value_write, but a string is quoted and escaped even at the top level. */
bool lz_value_write_debug(const LzValue *value, const LzWriter *writer);

#endif
