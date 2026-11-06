#include "lizard/value.h"
#include "lizard/error.h"

#include "lizard/array.h"
#include "lizard/class.h"
#include "lizard/dict.h"
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
    const LzObject *object; /* an array or a dict */
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

static bool is_string_value(const LzValue *value) {
    return value->type == LZ_VAL_OBJECT && value->as.object->type == LZ_OBJ_STRING;
}

static bool write_entry(const LzValue *value, const LzWriter *writer) {
    if (lz_value_is_instance(value)) {
        return lz_instance_write(lz_value_as_instance(value), writer, true);
    }
    if (is_string_value(value)) {
        return write_quoted((const LzString *)value->as.object, writer);
    }
    if (lz_value_is_instance(value)) {
        return lz_instance_write(lz_value_as_instance(value), writer, true);
    }
    if (lz_value_is_error(value)) {
        return lz_error_write_text((const LzError *)value->as.object, writer, true);
    }
    return lz_value_write(value, writer);
}

/*
 * Iterative, so a structure nested a million levels deep cannot overflow the C
 * stack. Strings inside an array or a dict are quoted; at the top level they are not.
 */
static bool write_container(const LzObject *root, const LzWriter *writer) {
    size_t capacity = 16;
    size_t depth = 0;
    WriteFrame *frames = malloc(capacity * sizeof *frames);
    if (frames == NULL) {
        return false;
    }
    bool root_dict = root->type == LZ_OBJ_DICT;
    bool ok = lz_writer_write_cstr(writer, root_dict ? "{" : "[");
    frames[depth++] = (WriteFrame){root, 0};

    while (ok && depth > 0) {
        WriteFrame *frame = &frames[depth - 1];
        bool dict = frame->object->type == LZ_OBJ_DICT;
        size_t count = dict ? ((const LzDict *)frame->object)->count
                            : ((const LzArray *)frame->object)->count;
        if (frame->next == count) {
            ok = lz_writer_write_cstr(writer, dict ? "}" : "]");
            depth--;
            continue;
        }
        const LzValue *item = NULL;
        if (dict) {
            const LzDictEntry *entry = &((const LzDict *)frame->object)->entries[frame->next];
            if (frame->next > 0) {
                ok = lz_writer_write_cstr(writer, ", ");
            }
            ok = ok && write_entry(&entry->key, writer) && lz_writer_write_cstr(writer, ": ");
            item = &entry->value;
        } else {
            if (frame->next > 0) {
                ok = lz_writer_write_cstr(writer, ", ");
            }
            item = &((const LzArray *)frame->object)->items[frame->next];
        }
        frame->next++;
        if (!ok) {
            break;
        }
        if (lz_value_is_container(item) && !lz_value_is_instance(item)) {
            if (depth == capacity) {
                WriteFrame *grown = realloc(frames, capacity * 2 * sizeof *frames);
                if (grown == NULL) {
                    ok = false;
                    break;
                }
                frames = grown;
                capacity *= 2;
            }
            bool inner_dict = item->as.object->type == LZ_OBJ_DICT;
            frames[depth++] = (WriteFrame){item->as.object, 0};
            ok = lz_writer_write_cstr(writer, inner_dict ? "{" : "[");
        } else {
            ok = write_entry(item, writer);
        }
    }
    free(frames);
    return ok;
}

static bool write_dict(const LzDict *dict, const LzWriter *writer) {
    return write_container(&dict->base, writer);
}

static bool write_object(const LzObject *object, const LzWriter *writer) {
    switch (object->type) {
    case LZ_OBJ_STRING: {
        const LzString *string = (const LzString *)object;
        return lz_writer_write(writer, string->data, string->length);
    }
    case LZ_OBJ_ARRAY:
        return write_container(object, writer);
    case LZ_OBJ_DICT:
        return write_dict((const LzDict *)object, writer);
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
    case LZ_OBJ_ERROR:
        return lz_error_write_text((const LzError *)object, writer, false);
    case LZ_OBJ_CLASS: {
        const LzClass *cls = (const LzClass *)object;
        return lz_writer_write_cstr(writer, cls->is_trait ? "<trait " : "<class ") &&
               lz_writer_write(writer, cls->name->data, cls->name->length) &&
               lz_writer_write_cstr(writer, ">");
    }
    case LZ_OBJ_INSTANCE:
        return lz_instance_write((const LzInstance *)object, writer, false);
    case LZ_OBJ_BOUND: {
        const LzBound *bound = (const LzBound *)object;
        if (lz_value_is_closure(&bound->callee) && !bound->skip_first) {
            const LzFunction *function = lz_value_as_closure(&bound->callee)->function;
            return lz_writer_write_cstr(writer, "<method ") &&
                   lz_writer_write_cstr(writer, lz_function_display_name(function)) &&
                   lz_writer_write_cstr(writer, ">");
        }
        return lz_value_write(&bound->callee, writer);
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
    case LZ_OBJ_DICT: {
        LzDict *dict = (LzDict *)object;
        for (size_t i = 0; i < dict->count; i++) {
            lz_drop_container_holder(&dict->entries[i].value);
            lz_value_release(heap, &dict->entries[i].key);
            lz_value_release(heap, &dict->entries[i].value);
        }
        free(dict->entries);
        free(dict->slots);
        lz_heap_uncharge(heap, dict->capacity * sizeof(LzDictEntry));
        lz_heap_uncharge(heap, dict->slot_capacity * sizeof(uint32_t));
        break;
    }
    case LZ_OBJ_ERROR: {
        LzError *error = (LzError *)object;
        lz_object_release(heap, &error->message->base);
        lz_object_release(heap, &error->kind_name->base);
        if (error->trace != NULL) {
            lz_object_release(heap, &error->trace->base);
        }
        lz_value_release(heap, &error->cause);
        if (error->payload != NULL) {
            error->payload->owner_error = NULL;
            lz_object_release(heap, &error->payload->base);
        }
        lz_drop_container_holder(&error->data);
        lz_value_release(heap, &error->data);
        break;
    }
    case LZ_OBJ_CLASS:
        lz_class_finalize(heap, (LzClass *)object);
        break;
    case LZ_OBJ_INSTANCE:
        lz_instance_finalize(heap, (LzInstance *)object);
        break;
    case LZ_OBJ_BOUND:
        lz_bound_finalize(heap, (LzBound *)object);
        break;
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
        if (upvalue->watch.next != NULL) {
            lz_watch_unlink(&upvalue->watch); /* Skink must never poll a dead upvalue */
        }
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

static size_t equal_count(const LzObject *object) {
    switch (object->type) {
    case LZ_OBJ_DICT:
        return ((const LzDict *)object)->count;
    case LZ_OBJ_INSTANCE:
        return ((const LzInstance *)object)->cls->field_count;
    default:
        return ((const LzArray *)object)->count;
    }
}

static const LzValue *equal_item(const LzObject *object, size_t index) {
    if (object->type == LZ_OBJ_INSTANCE) {
        return &((const LzInstance *)object)->fields[index];
    }
    return &((const LzArray *)object)->items[index];
}

typedef struct EqualFrame {
    const LzObject *left;
    const LzObject *right;
    size_t next;
} EqualFrame;

/* Does right hold left's key, with a value equal to left's? Dicts compare by content. */
static bool pair_equal(const LzValue *key, const LzValue *value, const LzObject *right,
                       uint64_t *used, uint64_t budget, bool *exhausted) {
    const LzValue *other = lz_dict_get((const LzDict *)right, key);
    if (other == NULL) {
        return false;
    }
    uint64_t work = budget - *used;
    bool same = lz_value_equal_budget(value, other, &work);
    *used += work;
    if (work == UINT64_MAX) {
        *exhausted = true;
        return false;
    }
    return same;
}

bool lz_value_equal_budget(const LzValue *left, const LzValue *right, uint64_t *work) {
    uint64_t budget = *work;
    uint64_t used = 0;
    *work = 0;
    if (left->type == LZ_VAL_OBJECT && left->as.object->type == LZ_OBJ_INSTANCE) {
        bool same = false;
        if (lz_class_equal_hook(left, right, &same)) {
            return same; /* the class's own `operator ==` answered */
        }
    }

    if (!lz_value_is_container(left) || !lz_value_is_container(right) ||
        left->as.object == right->as.object) {
        return equal_leaf(left, right);
    }
    if (left->as.object->type != right->as.object->type) {
        return false;
    }
    bool left_dict = left->as.object->type == LZ_OBJ_DICT;
    if (left->as.object->type == LZ_OBJ_INSTANCE &&
        lz_value_as_instance(left)->cls != lz_value_as_instance(right)->cls) {
        return false;
    }
    if (left_dict) {
        /* Order does not matter: both must hold the same keys with equal values. */
        const LzDict *a = lz_value_as_dict(left);
        const LzDict *b = lz_value_as_dict(right);
        if (a->count != b->count) {
            return false;
        }
        for (size_t i = 0; i < a->count; i++) {
            if (++used > budget) {
                *work = UINT64_MAX;
                return false;
            }
            bool exhausted = false;
            bool same = pair_equal(&a->entries[i].key, &a->entries[i].value, &b->base, &used, budget,
                                   &exhausted);
            if (exhausted) {
                *work = UINT64_MAX;
                return false;
            }
            if (!same) {
                *work = used;
                return false;
            }
        }
        *work = used;
        return true;
    }

    const LzObject *x = left->as.object;
    const LzObject *y = right->as.object;
    if (equal_count(x) != equal_count(y)) {
        return false;
    }

    size_t capacity = 16;
    size_t depth = 0;
    EqualFrame *frames = malloc(capacity * sizeof *frames);
    if (frames == NULL) {
        return false;
    }
    frames[depth++] = (EqualFrame){x, y, 0};

    bool same = true;
    while (same && depth > 0) {
        EqualFrame *frame = &frames[depth - 1];
        bool frame_dict = frame->left->type == LZ_OBJ_DICT;
        size_t count = equal_count(frame->left);
        if (frame->next == count) {
            depth--;
            continue;
        }
        if (frame_dict) {
            /* A dict compares by key, in no order: look the other side up. */
            const LzDictEntry *entry = &((const LzDict *)frame->left)->entries[frame->next];
            frame->next++;
            if (++used > budget) {
                same = false;
                used = UINT64_MAX;
                break;
            }
            bool exhausted = false;
            if (!pair_equal(&entry->key, &entry->value, frame->right, &used, budget, &exhausted)) {
                same = false;
                used = exhausted ? UINT64_MAX : used;
                break;
            }
            continue;
        }
        const LzValue *a = equal_item(frame->left, frame->next);
        const LzValue *b = equal_item(frame->right, frame->next);
        frame->next++;
        if (++used > budget) {
            same = false;
            used = UINT64_MAX;
            break;
        }
        if (a->type == LZ_VAL_OBJECT && a->as.object->type == LZ_OBJ_INSTANCE) {
            bool answer = false;
            if (lz_class_equal_hook(a, b, &answer)) {
                if (!answer) {
                    same = false;
                }
                continue;
            }
        }
        if (lz_value_is_container(a) && lz_value_is_container(b) && a->as.object != b->as.object) {
            if (a->as.object->type != b->as.object->type) {
                same = false;
                break;
            }
            if (a->as.object->type == LZ_OBJ_INSTANCE &&
                lz_value_as_instance(a)->cls != lz_value_as_instance(b)->cls) {
                same = false;
                break;
            }
            size_t inner = equal_count(a->as.object);
            size_t other = equal_count(b->as.object);
            if (inner != other) {
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
            frames[depth++] = (EqualFrame){a->as.object, b->as.object, 0};
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

bool lz_value_write_debug(const LzValue *value, const LzWriter *writer) {
    if (value->type == LZ_VAL_OBJECT && value->as.object->type == LZ_OBJ_STRING) {
        return write_quoted((const LzString *)value->as.object, writer);
    }
    if (lz_value_is_instance(value)) {
        return lz_instance_write(lz_value_as_instance(value), writer, true);
    }
    if (lz_value_is_error(value)) {
        return lz_error_write_text((const LzError *)value->as.object, writer, true);
    }
    return lz_value_write(value, writer);
}
