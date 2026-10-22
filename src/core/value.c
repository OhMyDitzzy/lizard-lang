#include "lizard/value.h"

#include "lizard/array.h"
#include "lizard/function.h"
#include "lizard/native.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

LzValue lz_value_nil(void) {
    LzValue value = {.type = LZ_VAL_NIL};
    return value;
}

LzValue lz_value_bool(bool boolean) {
    LzValue value = {.type = LZ_VAL_BOOL, .as.boolean = boolean};
    return value;
}

LzValue lz_value_int(int64_t integer) {
    LzValue value = {.type = LZ_VAL_INT, .as.integer = integer};
    return value;
}

LzValue lz_value_float(double number) {
    LzValue value = {.type = LZ_VAL_FLOAT, .as.number = number};
    return value;
}

LzValue lz_value_object(LzObject *object) {
    LzValue value = {.type = LZ_VAL_OBJECT, .as.object = object};
    return value;
}

uint32_t lz_hash_bytes(const char *chars, size_t length) {
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < length; i++) {
        hash ^= (uint8_t)chars[i];
        hash *= 16777619u;
    }
    return hash;
}

LzString *lz_string_new(LzHeap *heap, const char *chars, size_t length) {
    LzString *string = lz_heap_alloc(heap, LZ_OBJ_STRING, sizeof(LzString) + length + 1);
    if (string == NULL) {
        return NULL;
    }
    string->length = length;
    if (length > 0) {
        memcpy(string->data, chars, length);
    }
    string->data[length] = '\0';
    string->hash = lz_hash_bytes(string->data, length);
    return string;
}

LzString *lz_string_concat(LzHeap *heap, const LzString *a, const LzString *b) {
    size_t length = a->length + b->length;
    if (length < a->length || length > SIZE_MAX - sizeof(LzString) - 1) {
        return NULL;
    }

    LzString *string = lz_heap_alloc(heap, LZ_OBJ_STRING, sizeof(LzString) + length + 1);
    if (string == NULL) {
        return NULL;
    }
    string->length = length;
    memcpy(string->data, a->data, a->length);
    memcpy(string->data + a->length, b->data, b->length);
    string->data[length] = '\0';
    string->hash = lz_hash_bytes(string->data, length);
    return string;
}

void lz_value_retain(const LzValue *value) {
    if (value->type == LZ_VAL_OBJECT) {
        lz_object_retain(value->as.object);
    }
}

void lz_value_release(LzHeap *heap, const LzValue *value) {
    if (value->type == LZ_VAL_OBJECT) {
        lz_object_release(heap, value->as.object);
    }
}

static bool write_float(double number, const LzWriter *writer) {
    char text[40];

    /* printf spells these differently per platform (and gives NaN a sign): fix the text. */
    if (isnan(number)) {
        return lz_writer_write_cstr(writer, "nan");
    }
    if (isinf(number)) {
        return lz_writer_write_cstr(writer, number < 0 ? "-inf" : "inf");
    }

    /* Shortest precision that round-trips, so 0.1 prints as 0.1 and not 0.10000000000000001. */
    for (int precision = 15; precision <= 17; precision++) {
        (void)snprintf(text, sizeof text, "%.*g", precision, number);
        if (strtod(text, NULL) == number) {
            break;
        }
    }

    if (!lz_writer_write_cstr(writer, text)) {
        return false;
    }
    bool looks_integral = text[strspn(text, "-0123456789")] == '\0';
    return !looks_integral || lz_writer_write_cstr(writer, ".0");
}

typedef struct WriteFrame {
    const LzArray *array;
    size_t next;
} WriteFrame;

static bool write_quoted(const LzString *string, const LzWriter *writer) {
    if (!lz_writer_write_cstr(writer, "\"")) {
        return false;
    }
    for (size_t i = 0; i < string->length; i++) {
        unsigned char c = (unsigned char)string->data[i];
        char escape[8];
        const char *text = NULL;
        switch (c) {
        case '"':
            text = "\\\"";
            break;
        case '\\':
            text = "\\\\";
            break;
        case '\n':
            text = "\\n";
            break;
        case '\t':
            text = "\\t";
            break;
        case '\r':
            text = "\\r";
            break;
        default:
            if (c < 0x20 || c == 0x7F) {
                (void)snprintf(escape, sizeof escape, "\\x%02X", (unsigned)c);
                text = escape;
            }
            break;
        }
        bool ok = text != NULL ? lz_writer_write_cstr(writer, text)
                               : lz_writer_write(writer, string->data + i, 1);
        if (!ok) {
            return false;
        }
    }
    return lz_writer_write_cstr(writer, "\"");
}

/*
 * Iterative, so an array nested a million levels deep cannot overflow the C
 * stack. Strings inside an array are quoted; at the top level they are not.
 */
static bool write_array(const LzArray *root, const LzWriter *writer) {
    size_t capacity = 16;
    size_t depth = 0;
    WriteFrame *frames = malloc(capacity * sizeof *frames);
    if (frames == NULL) {
        return false;
    }
    bool ok = lz_writer_write_cstr(writer, "[");
    frames[depth++] = (WriteFrame){root, 0};

    while (ok && depth > 0) {
        WriteFrame *frame = &frames[depth - 1];
        if (frame->next == frame->array->count) {
            ok = lz_writer_write_cstr(writer, "]");
            depth--;
            continue;
        }
        const LzValue *item = &frame->array->items[frame->next];
        if (frame->next > 0) {
            ok = lz_writer_write_cstr(writer, ", ");
        }
        frame->next++;
        if (!ok) {
            break;
        }
        if (lz_value_is_array(item)) {
            if (depth == capacity) {
                WriteFrame *grown = realloc(frames, capacity * 2 * sizeof *frames);
                if (grown == NULL) {
                    ok = false;
                    break;
                }
                frames = grown;
                capacity *= 2;
            }
            frames[depth++] = (WriteFrame){lz_value_as_array(item), 0};
            ok = lz_writer_write_cstr(writer, "[");
        } else if (item->type == LZ_VAL_OBJECT && item->as.object->type == LZ_OBJ_STRING) {
            ok = write_quoted((const LzString *)item->as.object, writer);
        } else {
            ok = lz_value_write(item, writer);
        }
    }
    free(frames);
    return ok;
}

static bool write_object(const LzObject *object, const LzWriter *writer) {
    switch (object->type) {
    case LZ_OBJ_STRING: {
        const LzString *string = (const LzString *)object;
        return lz_writer_write(writer, string->data, string->length);
    }
    case LZ_OBJ_ARRAY:
        return write_array((const LzArray *)object, writer);
    case LZ_OBJ_NATIVE: {
        const LzNative *native = (const LzNative *)object;
        return lz_writer_write_cstr(writer, "<native ") &&
               lz_writer_write(writer, native->name->data, native->name->length) &&
               lz_writer_write_cstr(writer, ">");
    }
    case LZ_OBJ_CLOSURE: {
        const LzFunction *function = ((const LzClosure *)object)->function;
        if (function->name == NULL) {
            return lz_writer_write_cstr(writer, "<fn>");
        }
        return lz_writer_write_cstr(writer, "<fn ") &&
               lz_writer_write(writer, function->name->data, function->name->length) &&
               lz_writer_write_cstr(writer, ">");
    }
    case LZ_OBJ_FUNCTION:
        return lz_writer_write_cstr(writer, "<function code>");
    case LZ_OBJ_UPVALUE:
        return lz_writer_write_cstr(writer, "<upvalue>");
    }
    return false;
}

void lz_object_finalize(LzHeap *heap, LzObject *object) {
    switch (object->type) {
    case LZ_OBJ_STRING:
        break;
    case LZ_OBJ_NATIVE:
        lz_object_release(heap, &((LzNative *)object)->name->base);
        break;
    case LZ_OBJ_ARRAY: {
        LzArray *array = (LzArray *)object;
        for (size_t i = 0; i < array->count; i++) {
            lz_array_drop_holder(&array->items[i]);
            lz_value_release(heap, &array->items[i]);
        }
        free(array->items);
        lz_heap_uncharge(heap, array->capacity * sizeof(LzValue));
        break;
    }
    case LZ_OBJ_FUNCTION:
        lz_function_free_storage(heap, (LzFunction *)object, true);
        break;
    case LZ_OBJ_CLOSURE: {
        LzClosure *closure = (LzClosure *)object;
        for (size_t i = 0; i < closure->upvalue_count; i++) {
            if (closure->upvalues[i] != NULL) {
                lz_object_release(heap, &closure->upvalues[i]->base);
            }
        }
        lz_object_release(heap, &closure->function->base);
        break;
    }
    case LZ_OBJ_UPVALUE: {
        LzUpvalue *upvalue = (LzUpvalue *)object;
        if (upvalue->location == &upvalue->closed) {
            lz_value_release(heap, &upvalue->closed);
        }
        break;
    }
    }
}

bool lz_value_is_truthy(const LzValue *value) {
    if (value->type == LZ_VAL_NIL) {
        return false;
    }
    return value->type != LZ_VAL_BOOL || value->as.boolean;
}

static LzOrder order_doubles(double a, double b) {
    if (a < b) {
        return LZ_ORDER_LESS;
    }
    if (a > b) {
        return LZ_ORDER_GREATER;
    }
    return a == b ? LZ_ORDER_EQUAL : LZ_ORDER_UNORDERED;
}

static LzOrder order_int_float(int64_t integer, double number) {
    if (isnan(number)) {
        return LZ_ORDER_UNORDERED;
    }
    if (number >= 9223372036854775808.0) {
        return LZ_ORDER_LESS;
    }
    if (number < -9223372036854775808.0) {
        return LZ_ORDER_GREATER;
    }

    int64_t truncated = (int64_t)number;
    if (integer < truncated) {
        return LZ_ORDER_LESS;
    }
    if (integer > truncated) {
        return LZ_ORDER_GREATER;
    }

    double fraction = number - (double)truncated;
    if (fraction > 0) {
        return LZ_ORDER_LESS;
    }
    return fraction < 0 ? LZ_ORDER_GREATER : LZ_ORDER_EQUAL;
}

static LzOrder flip(LzOrder order) {
    if (order == LZ_ORDER_LESS) {
        return LZ_ORDER_GREATER;
    }
    return order == LZ_ORDER_GREATER ? LZ_ORDER_LESS : order;
}

LzOrder lz_number_order(const LzValue *left, const LzValue *right) {
    if (left->type == LZ_VAL_INT && right->type == LZ_VAL_INT) {
        if (left->as.integer == right->as.integer) {
            return LZ_ORDER_EQUAL;
        }
        return left->as.integer < right->as.integer ? LZ_ORDER_LESS : LZ_ORDER_GREATER;
    }
    if (left->type == LZ_VAL_FLOAT && right->type == LZ_VAL_FLOAT) {
        return order_doubles(left->as.number, right->as.number);
    }
    if (left->type == LZ_VAL_INT) {
        return order_int_float(left->as.integer, right->as.number);
    }
    return flip(order_int_float(right->as.integer, left->as.number));
}

static bool is_number_value(const LzValue *value) {
    return value->type == LZ_VAL_INT || value->type == LZ_VAL_FLOAT;
}

/* Equality of everything but the contents of arrays (an array equals itself only). */
static bool equal_leaf(const LzValue *left, const LzValue *right) {
    if (is_number_value(left) && is_number_value(right)) {
        return lz_number_order(left, right) == LZ_ORDER_EQUAL;
    }
    if (left->type != right->type) {
        return false;
    }

    switch (left->type) {
    case LZ_VAL_NIL:
        return true;
    case LZ_VAL_BOOL:
        return left->as.boolean == right->as.boolean;
    case LZ_VAL_OBJECT: {
        const LzObject *a = left->as.object;
        const LzObject *b = right->as.object;
        if (a == b) {
            return true;
        }
        if (a->type == LZ_OBJ_STRING && b->type == LZ_OBJ_STRING) {
            const LzString *first = (const LzString *)a;
            const LzString *second = (const LzString *)b;
            return first->length == second->length &&
                   memcmp(first->data, second->data, first->length) == 0;
        }
        return false;
    }
    case LZ_VAL_INT:
    case LZ_VAL_FLOAT:
        break;
    }
    return false;
}

typedef struct EqualFrame {
    const LzArray *left;
    const LzArray *right;
    size_t next;
} EqualFrame;

bool lz_value_equal_budget(const LzValue *left, const LzValue *right, uint64_t *work) {
    uint64_t budget = *work;
    uint64_t used = 0;
    *work = 0;

    if (!lz_value_is_array(left) || !lz_value_is_array(right) ||
        left->as.object == right->as.object) {
        return equal_leaf(left, right);
    }
    if (lz_value_as_array(left)->count != lz_value_as_array(right)->count) {
        return false;
    }

    size_t capacity = 16;
    size_t depth = 0;
    EqualFrame *frames = malloc(capacity * sizeof *frames);
    if (frames == NULL) {
        return false;
    }
    frames[depth++] = (EqualFrame){lz_value_as_array(left), lz_value_as_array(right), 0};

    bool same = true;
    while (same && depth > 0) {
        EqualFrame *frame = &frames[depth - 1];
        if (frame->next == frame->left->count) {
            depth--;
            continue;
        }
        const LzValue *a = &frame->left->items[frame->next];
        const LzValue *b = &frame->right->items[frame->next];
        frame->next++;
        if (++used > budget) {
            used = UINT64_MAX;
            same = false;
            break;
        }
        if (lz_value_is_array(a) && lz_value_is_array(b) && a->as.object != b->as.object) {
            const LzArray *x = lz_value_as_array(a);
            const LzArray *y = lz_value_as_array(b);
            if (x->count != y->count) {
                same = false;
                break;
            }
            if (depth == capacity) {
                EqualFrame *grown = realloc(frames, capacity * 2 * sizeof *frames);
                if (grown == NULL) {
                    same = false;
                    break;
                }
                frames = grown;
                capacity *= 2;
            }
            frames[depth++] = (EqualFrame){x, y, 0};
        } else if (!equal_leaf(a, b)) {
            same = false;
        }
    }
    free(frames);
    *work = used;
    return same;
}

bool lz_value_equal(const LzValue *left, const LzValue *right) {
    uint64_t work = UINT64_MAX;
    return lz_value_equal_budget(left, right, &work);
}

bool lz_value_write(const LzValue *value, const LzWriter *writer) {
    switch (value->type) {
    case LZ_VAL_NIL:
        return lz_writer_write_cstr(writer, "nil");
    case LZ_VAL_BOOL:
        return lz_writer_write_cstr(writer, value->as.boolean ? "true" : "false");
    case LZ_VAL_INT: {
        char text[32];
        (void)snprintf(text, sizeof text, "%" PRId64, value->as.integer);
        return lz_writer_write_cstr(writer, text);
    }
    case LZ_VAL_FLOAT:
        return write_float(value->as.number, writer);
    case LZ_VAL_OBJECT:
        return write_object(value->as.object, writer);
    }
    return false;
}
