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

/* Writes the user-visible text form of a value. Returns false on an output error. */
bool lz_value_write(const LzValue *value, const LzWriter *writer);

#endif
