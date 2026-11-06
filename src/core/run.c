#include "lizard/vm.h"

#include "lizard/arith.h"
#include "lizard/array.h"
#include "lizard/class.h"
#include "lizard/compare.h"
#include "lizard/dict.h"
#include "lizard/error.h"
#include "lizard/native.h"

#include <stdio.h>
#include <string.h>

static bool read_u16(const LzChunk *chunk, size_t *ip, uint16_t *out) {
    if (*ip + 2 > chunk->count) {
        return false;
    }
    *out = (uint16_t)((chunk->code[*ip] << 8) | chunk->code[*ip + 1]);
    *ip += 2;
    return true;
}

static LzStatus malformed(LzVM *vm, const char *what) {
    return lz_vm_set_error(vm, LZ_ERR_RUNTIME, "malformed bytecode: %s", what);
}

static LzStatus push(LzVM *vm, LzValue value) {
    if (vm->stack_count >= LZ_STACK_MAX) {
        return lz_vm_set_error(vm, LZ_ERR_STACK_OVERFLOW, "stack overflow");
    }
    lz_value_retain(&value);
    vm->stack[vm->stack_count++] = value;
    return LZ_OK;
}

static void pop(LzVM *vm) {
    vm->stack_count--;
    lz_value_release(&vm->heap, &vm->stack[vm->stack_count]);
}

static LzStatus read_constant(LzVM *vm, const LzChunk *chunk, size_t *ip, const LzValue **out) {
    uint16_t index;
    if (!read_u16(chunk, ip, &index)) {
        return malformed(vm, "truncated operand");
    }
    if (index >= chunk->constant_count) {
        return malformed(vm, "constant index out of range");
    }
    *out = &chunk->constants[index];
    return LZ_OK;
}

static LzStatus read_name(LzVM *vm, const LzChunk *chunk, size_t *ip, LzString **name) {
    const LzValue *constant = NULL;
    LzStatus status = read_constant(vm, chunk, ip, &constant);
    if (status != LZ_OK) {
        return status;
    }
    if (constant->type != LZ_VAL_OBJECT || constant->as.object->type != LZ_OBJ_STRING) {
        return malformed(vm, "name operand is not a string");
    }
    *name = (LzString *)constant->as.object;
    return LZ_OK;
}

/* What a failed check was about, for the message: an argument, a returned value, or nothing. */
typedef enum CheckSubject { CHECK_VALUE, CHECK_PARAM, CHECK_RETURN } CheckSubject;

static void describe_subject(const LzVM *vm, CheckSubject subject, size_t slot, char *out, size_t size) {
    out[0] = '\0';
    const LzCallFrame *frame = &vm->frames[vm->frame_count - 1];
    if (subject == CHECK_VALUE || frame->closure == NULL) {
        return;
    }
    const LzFunction *function = frame->closure->function;
    if (subject == CHECK_PARAM) {
        const char *name = slot < lz_function_slots(function) ? function->param_names[slot]->data : "?";
        (void)snprintf(out, size, "argument '%s' of %s()", name, lz_function_display_name(function));
    } else {
        (void)snprintf(out, size, "return value of %s()", lz_function_display_name(function));
    }
}

static LzStatus type_mismatch(LzVM *vm, const LzType *type, const LzValue *value,
                              CheckSubject subject, size_t slot) {
    LzBuffer expected = {0};
    LzBuffer actual = {0};
    LzWriter expected_out = lz_writer_from_buffer(&expected);
    LzWriter actual_out = lz_writer_from_buffer(&actual);

    lz_type_describe(type, &expected_out);
    if (lz_value_is_array(value) || lz_value_is_dict(value)) {
        LzType shown = lz_type_of_value(value);
        lz_type_describe(&shown, &actual_out);
    } else if (type->literal_count > 0) {
        lz_value_describe(value, &actual_out);
    } else {
        (void)lz_writer_write_cstr(&actual_out, lz_value_kind_name(value));
        if (lz_value_is_instance(value) && (type->kinds & LZ_KIND_INSTANCE) != 0 && type->class_id != 0 &&
            type->class_name == lz_value_as_instance(value)->cls->name) {
            /* Same name, different class: it was defined again, and this object is from before. */
            (void)lz_writer_write_cstr(&actual_out, " (made by an earlier definition of that class)");
        }
    }

    char what[160];
    describe_subject(vm, subject, slot, what, sizeof what);
    if (subject == CHECK_PARAM && slot == 0 && (type->kinds & LZ_KIND_INSTANCE) != 0 && type->class_name != NULL) {
        const LzCallFrame *frame = &vm->frames[vm->frame_count - 1];
        if (frame->closure != NULL && frame->closure->function->owner == type->class_name &&
            frame->closure->function->param_count > 0 &&
            strcmp(frame->closure->function->param_names[0]->data, "self") == 0) {
            LzStatus own = lz_vm_set_error(vm, LZ_ERR_TYPE, "%s() needs %s %s as its first argument, got %s",
                                           lz_function_display_name(frame->closure->function),
                                           strchr("AEIOUaeiou", type->class_name->data[0]) != NULL ? "an" : "a",
                                           type->class_name->data, actual.data != NULL ? actual.data : "?");
            lz_buffer_free(&expected);
            lz_buffer_free(&actual);
            return own;
        }
    }
    LzStatus status = lz_vm_set_error(vm, LZ_ERR_TYPE, "type error: %s%sexpected %s, got %s", what,
                                      what[0] != '\0' ? ": " : "",
                                      expected.data != NULL ? expected.data : "?",
                                      actual.data != NULL ? actual.data : "?");
    lz_buffer_free(&expected);
    lz_buffer_free(&actual);
    return status;
}

/*
 * Does the value fit the type? An array is checked item by item and then restricted to
 * the item type, which costs fuel in proportion to its size.
 */
static LzStatus check_value(LzVM *vm, const LzType *type, const LzValue *value, CheckSubject subject,
                            size_t slot) {
    LzStatus status = LZ_OK;
    bool fits;
    if (lz_value_is_array(value)) {
        status = lz_vm_charge_fuel(vm, lz_array_cost(lz_value_as_array(value)->count));
        fits = (type->kinds & LZ_KIND_ARRAY) != 0 && lz_array_conform(lz_value_as_array(value), type->element);
    } else {
        fits = lz_type_matches(type, value);
    }
    if (status == LZ_OK && !fits) {
        status = type_mismatch(vm, type, value, subject, slot);
    }
    return status;
}

static LzStatus array_new(LzVM *vm) {
    LzArray *array = lz_array_new(&vm->heap, 0);
    if (array == NULL) {
        return lz_vm_out_of_memory(vm);
    }
    LzStatus status = push(vm, lz_value_object(&array->base));
    lz_object_release(&vm->heap, &array->base); /* the stack holds its own reference now */
    return status;
}

/* OP_DICT n: the key and value pairs are on top, in source order. */
static LzStatus dict_new(LzVM *vm, uint16_t count) {
    size_t needed = (size_t)count * 2;
    if (vm->stack_count < needed) {
        return malformed(vm, "stack underflow");
    }
    LzDict *dict = lz_dict_new(&vm->heap, count);
    if (dict == NULL) {
        return lz_vm_out_of_memory(vm);
    }
    LzValue result = lz_value_object(&dict->base);
    size_t base = vm->stack_count - needed;
    LzStatus status = LZ_OK;
    for (size_t i = 0; status == LZ_OK && i < count; i++) {
        status = lz_dict_set(vm, dict, &vm->stack[base + i * 2], &vm->stack[base + i * 2 + 1]);
    }
    for (size_t i = 0; i < needed; i++) {
        pop(vm);
    }
    if (status == LZ_OK) {
        status = push(vm, result);
    }
    lz_object_release(&vm->heap, &dict->base);
    return status;
}

static LzStatus array_push(LzVM *vm) {
    if (vm->stack_count < 2) {
        return malformed(vm, "stack underflow");
    }
    const LzValue *target = &vm->stack[vm->stack_count - 2];
    if (!lz_value_is_array(target)) {
        return malformed(vm, "array expected");
    }
    LzStatus status = lz_array_push(vm, lz_value_as_array(target), &vm->stack[vm->stack_count - 1]);
    if (status == LZ_OK) {
        pop(vm);
    }
    return status;
}

static LzStatus not_indexable(LzVM *vm, const LzValue *value) {
    return lz_vm_set_error(vm, LZ_ERR_RUNTIME, "cannot index a value of type %s",
                           lz_value_kind_name(value));
}

/*
 * d[key]: the item, or "key \"x\" not found" with a hint drawn from the dict's own
 * string keys. `op` is the instruction, so a compound store can suggest get().
 */
static LzStatus dict_index_op(LzVM *vm, const LzDict *dict, const LzValue *key) {
    LzStatus status = lz_dict_own_key(vm, key);
    if (status != LZ_OK) {
        return status;
    }
    const LzValue *stored = lz_dict_get(dict, key);
    if (stored == NULL) {
        return lz_vm_dict_key_error(vm, dict, key, NULL);
    }
    LzValue item = *stored;
    lz_value_retain(&item); /* keep it alive across the pops, which may free the dict */
    pop(vm);
    pop(vm);
    vm->stack[vm->stack_count++] = item;
    return LZ_OK;
}

static LzStatus index_op(LzVM *vm) {
    if (vm->stack_count < 2) {
        return malformed(vm, "stack underflow");
    }
    const LzValue *container = &vm->stack[vm->stack_count - 2];
    const LzValue *index = &vm->stack[vm->stack_count - 1];
    if (lz_value_is_dict(container)) {
        return dict_index_op(vm, lz_value_as_dict(container), index);
    }
    if (!lz_value_is_array(container)) {
        return not_indexable(vm, container);
    }
    const LzArray *array = lz_value_as_array(container);
    size_t position = 0;
    LzStatus status = lz_array_resolve_index(vm, index, array->count, &position);
    if (status != LZ_OK) {
        return status;
    }
    LzValue item = array->items[position];
    lz_value_retain(&item); /* keep it alive across the pops, which may free the array */
    pop(vm);
    pop(vm);
    vm->stack[vm->stack_count++] = item;
    return LZ_OK;
}

static bool text_at(const LzValue *value, const LzString **out) {
    if (value->type != LZ_VAL_OBJECT || value->as.object->type != LZ_OBJ_STRING) {
        return false;
    }
    *out = (const LzString *)value->as.object;
    return true;
}

/* The length in bytes of the code point at data[at], or 1 for a byte that is not valid UTF-8. */
static size_t code_point_length(const char *data, size_t length, size_t at) {
    unsigned char lead = (unsigned char)data[at];
    size_t size = lead < 0x80           ? 1
                  : (lead >> 5) == 0x6  ? 2
                  : (lead >> 4) == 0xE  ? 3
                  : (lead >> 3) == 0x1E ? 4
                                        : 1;
    if (at + size > length) {
        return 1;
    }
    for (size_t k = 1; k < size; k++) {
        if (((unsigned char)data[at + k] & 0xC0) != 0x80) {
            return 1;
        }
    }
    return size;
}

static LzStatus iter_length_op(LzVM *vm) {
    if (vm->stack_count == 0) {
        return malformed(vm, "stack underflow");
    }
    const LzValue *value = &vm->stack[vm->stack_count - 1];
    const LzString *text = NULL;
    int64_t length = 0;
    if (text_at(value, &text)) {
        length = (int64_t)text->length;
    } else if (lz_value_is_array(value)) {
        length = (int64_t)lz_value_as_array(value)->count;
    } else if (lz_value_is_dict(value)) {
        length = (int64_t)lz_value_as_dict(value)->count;
    } else {
        return lz_vm_set_error(vm, LZ_ERR_RUNTIME, "cannot iterate over %s",
                               lz_value_kind_name(value));
    }
    pop(vm);
    return push(vm, lz_value_int(length));
}

static LzStatus iter_next_op(LzVM *vm) {
    if (vm->stack_count < 2) {
        return malformed(vm, "stack underflow");
    }
    const LzValue *container = &vm->stack[vm->stack_count - 2];
    const LzValue *offset = &vm->stack[vm->stack_count - 1];
    if (offset->type != LZ_VAL_INT) {
        return malformed(vm, "loop offset is not an int");
    }
    int64_t at = offset->as.integer;
    const LzString *text = NULL;
    LzValue item = lz_value_nil();
    int64_t next = at + 1;
    LzString *piece = NULL;

    if (text_at(container, &text)) {
        if (at < 0 || (size_t)at >= text->length) {
            return lz_vm_set_error(vm, LZ_ERR_RUNTIME, "loop offset out of range");
        }
        size_t size = code_point_length(text->data, text->length, (size_t)at);
        piece = lz_string_new(&vm->heap, text->data + at, size);
        if (piece == NULL) {
            return lz_vm_out_of_memory(vm);
        }
        item = lz_value_object(&piece->base);
        next = at + (int64_t)size;
    } else if (lz_value_is_array(container)) {
        const LzArray *array = lz_value_as_array(container);
        if (at < 0 || (size_t)at >= array->count) {
            return lz_vm_set_error(vm, LZ_ERR_RUNTIME, "index out of range");
        }
        item = array->items[at];
    } else if (lz_value_is_dict(container)) {
        const LzDict *dict = lz_value_as_dict(container);
        if (at < 0 || (size_t)at >= dict->count) {
            return lz_vm_set_error(vm, LZ_ERR_RUNTIME, "index out of range");
        }
        item = dict->entries[at].key; /* `for k in d` walks the keys */
    } else {
        return lz_vm_set_error(vm, LZ_ERR_RUNTIME, "cannot iterate over %s",
                               lz_value_kind_name(container));
    }

    /*
     * The container slot stays, so the item is still held by the array; the retain only
     * covers the pops below, which could free it.
     */
    lz_value_retain(&item);
    pop(vm); /* the offset */
    LzStatus status = push(vm, item);
    if (status == LZ_OK) {
        status = push(vm, lz_value_int(next));
    }
    lz_value_release(&vm->heap, &item);
    if (piece != NULL) {
        lz_object_release(&vm->heap, &piece->base);
    }
    return status;
}

/* [x][off][round] -> [x][first][second][next]: an array or a string gives the round and
 * the item, a dict its key and its value. */
static LzStatus iter_pair_op(LzVM *vm) {
    if (vm->stack_count < 3) {
        return malformed(vm, "stack underflow");
    }
    const LzValue *container = &vm->stack[vm->stack_count - 3];
    const LzValue *offset = &vm->stack[vm->stack_count - 2];
    const LzValue *round = &vm->stack[vm->stack_count - 1];
    if (offset->type != LZ_VAL_INT || round->type != LZ_VAL_INT) {
        return malformed(vm, "loop offset is not an int");
    }
    int64_t at = offset->as.integer;
    LzValue first = *round;
    LzValue second = lz_value_nil();
    int64_t next = at + 1;
    const LzString *text = NULL;
    LzString *piece = NULL;

    if (lz_value_is_dict(container)) {
        const LzDict *dict = lz_value_as_dict(container);
        if (at < 0 || (size_t)at >= dict->count) {
            return lz_vm_set_error(vm, LZ_ERR_RUNTIME, "index out of range");
        }
        first = dict->entries[at].key;
        second = dict->entries[at].value;
    } else if (text_at(container, &text)) {
        if (at < 0 || (size_t)at >= text->length) {
            return lz_vm_set_error(vm, LZ_ERR_RUNTIME, "loop offset out of range");
        }
        size_t size = code_point_length(text->data, text->length, (size_t)at);
        piece = lz_string_new(&vm->heap, text->data + at, size);
        if (piece == NULL) {
            return lz_vm_out_of_memory(vm);
        }
        second = lz_value_object(&piece->base);
        next = at + (int64_t)size;
    } else if (lz_value_is_array(container)) {
        const LzArray *array = lz_value_as_array(container);
        if (at < 0 || (size_t)at >= array->count) {
            return lz_vm_set_error(vm, LZ_ERR_RUNTIME, "index out of range");
        }
        second = array->items[at];
    } else {
        return lz_vm_set_error(vm, LZ_ERR_RUNTIME, "cannot iterate over %s",
                               lz_value_kind_name(container));
    }

    lz_value_retain(&first);
    lz_value_retain(&second);
    pop(vm); /* the round */
    pop(vm); /* the offset */
    LzStatus status = push(vm, first);
    if (status == LZ_OK) {
        status = push(vm, second);
    }
    if (status == LZ_OK) {
        status = push(vm, lz_value_int(next));
    }
    lz_value_release(&vm->heap, &first);
    lz_value_release(&vm->heap, &second);
    if (piece != NULL) {
        lz_object_release(&vm->heap, &piece->base);
    }
    return status;
}

/* A dict may have its values changed while it is walked, but not its size. */
static LzStatus iter_check_op(LzVM *vm) {
    if (vm->stack_count < 2) {
        return malformed(vm, "stack underflow");
    }
    const LzValue *container = &vm->stack[vm->stack_count - 1];
    const LzValue *size = &vm->stack[vm->stack_count - 2];
    LzStatus status = LZ_OK;
    if (lz_value_is_dict(container) &&
        (size->type != LZ_VAL_INT || (size_t)size->as.integer != lz_value_as_dict(container)->count)) {
        status = lz_vm_set_error(vm, LZ_ERR_RUNTIME, "dict changed size during iteration");
    }
    pop(vm);
    pop(vm);
    return status;
}

/* "range bounds must be ints, got <kind>": names the first operand that is not an int. */
static bool range_bounds_int(LzVM *vm, const LzValue *start, const LzValue *end,
                             const LzValue *step) {
    const LzValue *offending = start;
    if (start->type != LZ_VAL_INT) {
        offending = start;
    } else if (end->type != LZ_VAL_INT) {
        offending = end;
    } else if (step->type != LZ_VAL_INT) {
        offending = step;
    } else {
        return true;
    }
    (void)lz_vm_set_error(vm, LZ_ERR_TYPE, "range bounds must be ints, got %s",
                          lz_value_kind_name(offending));
    return false;
}

static LzStatus range_test_op(LzVM *vm, bool inclusive) {
    if (vm->stack_count < 3) {
        return malformed(vm, "stack underflow");
    }
    const LzValue *counter = &vm->stack[vm->stack_count - 3];
    const LzValue *limit = &vm->stack[vm->stack_count - 2];
    const LzValue *step = &vm->stack[vm->stack_count - 1];
    if (!range_bounds_int(vm, counter, limit, step)) {
        return LZ_ERR_TYPE; /* already reported */
    }
    int64_t start = counter->as.integer;
    int64_t end = limit->as.integer;
    int64_t by = step->as.integer;
    if (by == 0) {
        return lz_vm_set_error(vm, LZ_ERR_RUNTIME, "range step cannot be 0");
    }
    bool inside = by > 0 ? (inclusive ? start <= end : start < end)
                         : (inclusive ? start >= end : start > end);
    pop(vm);
    pop(vm);
    pop(vm);
    return push(vm, lz_value_bool(inside));
}

/*
 * The last value the loop visits. It runs only when the range is not empty, so the
 * distance fits in 64 bits and the last value is one the loop reaches exactly.
 */
/* Is `needle` a substring of `hay`? Bytes, like contains(); the empty string is in everything. */
static bool bytes_contain(const LzString *hay, const LzString *needle) {
    if (needle->length == 0) {
        return true;
    }
    if (needle->length > hay->length) {
        return false;
    }
    size_t last = hay->length - needle->length;
    for (size_t i = 0; i <= last; i++) {
        const char *at = memchr(hay->data + i, needle->data[0], last - i + 1);
        if (at == NULL) {
            return false;
        }
        i = (size_t)(at - hay->data);
        if (memcmp(at, needle->data, needle->length) == 0) {
            return true;
        }
    }
    return false;
}

/*
 * `a in b`. An array holds a when one of its items == a (the rule of ==, so 1.0 in [1] is
 * true), a dict when a is one of its keys (a value that cannot be a key is simply absent),
 * a string when a is a substring. The caret of an error is on b, whatever the cause.
 */
static LzStatus in_op(LzVM *vm) {
    if (vm->stack_count < 2) {
        return malformed(vm, "stack underflow");
    }
    const LzValue *left = &vm->stack[vm->stack_count - 2];
    const LzValue *right = &vm->stack[vm->stack_count - 1];
    bool found = false;

    if (lz_value_is_array(right)) {
        const LzArray *array = lz_value_as_array(right);
        /* One unit of fuel per 8 items looked at, spent before the items are, so a huge array
         * cannot be walked once the budget is gone. */
        for (size_t i = 0; i < array->count && !found; i++) {
            if (i % 8 == 0) {
                LzStatus charged = lz_vm_charge_fuel(vm, 1);
                if (charged != LZ_OK) {
                    return charged;
                }
            }
            LzStatus status = lz_vm_values_equal(vm, left, &array->items[i], &found);
            if (status != LZ_OK) {
                return status;
            }
        }
    } else if (lz_value_is_dict(right)) {
        uint16_t kind = lz_kind_of(left);
        if ((kind & (LZ_KIND_STRING | LZ_KIND_INT | LZ_KIND_BOOL)) != 0) {
            found = lz_dict_get(lz_value_as_dict(right), left) != NULL;
        }
    } else if (right->type == LZ_VAL_OBJECT && right->as.object->type == LZ_OBJ_STRING) {
        const LzString *hay = (const LzString *)right->as.object;
        const LzString *needle = NULL;
        if (!text_at(left, &needle)) {
            return lz_vm_set_error(vm, LZ_ERR_TYPE,
                                   "the left side of 'in' must be a string when the right side "
                                   "is a string, got %s",
                                   lz_value_kind_name(left));
        }
        uint64_t width = needle->length < 64 ? needle->length / 8 + 1 : 9;
        LzStatus charged = lz_vm_charge_fuel(vm, ((uint64_t)hay->length / 64 + 1) * width);
        if (charged != LZ_OK) {
            return charged;
        }
        found = bytes_contain(hay, needle);
    } else {
        return lz_vm_set_error(vm, LZ_ERR_RUNTIME, "cannot use 'in' with %s",
                               lz_value_kind_name(right));
    }

    pop(vm);
    pop(vm);
    return push(vm, lz_value_bool(found));
}

/* `x in a..b` / `x in a..=b`: [x][a][b] -> bool, with the same answers as x >= a and x < b. */
static LzStatus in_range_op(LzVM *vm, bool inclusive, bool lenient) {
    if (vm->stack_count < 3) {
        return malformed(vm, "stack underflow");
    }
    const LzValue *value = &vm->stack[vm->stack_count - 3];
    const LzValue *low = &vm->stack[vm->stack_count - 2];
    const LzValue *high = &vm->stack[vm->stack_count - 1];
    if (!range_bounds_int(vm, low, high, high)) {
        return LZ_ERR_TYPE; /* already reported */
    }
    if (value->type != LZ_VAL_INT && value->type != LZ_VAL_FLOAT) {
        if (lenient) { /* a `case` range: what is not a number is not in it */
            pop(vm);
            pop(vm);
            pop(vm);
            return push(vm, lz_value_bool(false));
        }
        return lz_vm_set_error(vm, LZ_ERR_TYPE,
                               "the value tested with a range must be a number, got %s",
                               lz_value_kind_name(value));
    }
    LzValue above;
    LzValue below;
    LzStatus status = lz_compare_order(vm, LZ_OP_GREATER_EQUAL, value, low, &above);
    if (status == LZ_OK) {
        status = lz_compare_order(vm, inclusive ? LZ_OP_LESS_EQUAL : LZ_OP_LESS, value, high,
                                  &below);
    }
    if (status != LZ_OK) {
        return status;
    }
    bool inside = above.as.boolean && below.as.boolean;
    pop(vm);
    pop(vm);
    pop(vm);
    return push(vm, lz_value_bool(inside));
}

/* `??`: [x] -> [x][x is nil]. */
static LzStatus nil_test_op(LzVM *vm) {
    if (vm->stack_count == 0) {
        return malformed(vm, "stack underflow");
    }
    return push(vm, lz_value_bool(vm->stack[vm->stack_count - 1].type == LZ_VAL_NIL));
}

/* The error value a failure is turned into when a catch takes it: the thrown one, or a new one. */
static LzStatus error_for_failure(LzVM *vm, LzError **out) {
    if (vm->thrown.type == LZ_VAL_OBJECT) {
        *out = (LzError *)vm->thrown.as.object; /* the VM's reference moves to the caller */
        vm->thrown = lz_value_nil();
        return LZ_OK;
    }
    return lz_error_new(vm, (LzErrorKind)vm->error_kind, vm->error, strlen(vm->error), out);
}

/*
 * A failure at the instruction at byte `failed_ip` of the frame on top. If a catchable failure
 * meets a handler of a frame this loop runs (frame >= floor), cut back to the state the try began
 * in: close the upvalues above its stack height, drop the values and frames above, push the error
 * value and continue at the catch code. Returns false when nothing catches it (sandbox limits
 * never are), with *status as the failure the loop must return.
 */
static bool unwind_to_handler(LzVM *vm, LzStatus *status, size_t floor, size_t failed_ip,
                              const LzChunk **chunk_io, size_t *ip_io) {
    if (!lz_status_catchable(*status) || vm->handler_count == 0 || vm->frame_count == 0) {
        return false;
    }
    LzHandler handler = vm->handlers[vm->handler_count - 1];
    if (handler.frame < floor || handler.frame >= vm->frame_count) {
        return false;
    }
    const LzChunk *failing = vm->frames[vm->frame_count - 1].chunk;
    if (vm->error_offset == LZ_NO_OFFSET && failed_ip < failing->count) {
        vm->error_offset = failing->offsets[failed_ip];
    }
    if (!vm->trace_fixed) {
        lz_vm_capture_trace(vm);
    }
    LzError *error = NULL;
    LzStatus made = error_for_failure(vm, &error);
    if (made == LZ_OK) {
        made = lz_error_attach_trace(vm, error);
        if (made != LZ_OK) {
            lz_object_release(&vm->heap, &error->base);
        }
    }
    if (made != LZ_OK) {
        *status = made; /* the heap refused the error value: that is a limit, nothing to catch */
        return false;
    }

    vm->handler_count--;
    lz_vm_close_upvalues(vm, handler.stack_height);
    while (vm->stack_count > handler.stack_height) {
        vm->stack_count--;
        lz_value_release(&vm->heap, &vm->stack[vm->stack_count]);
    }
    vm->frame_count = handler.frame + 1;
    vm->trace_fixed = false;
    vm->trace_count = 0;
    vm->trace_total = 0;
    vm->error_offset = LZ_NO_OFFSET;
    vm->stack[vm->stack_count++] = lz_value_object(&error->base); /* the creation reference */
    *chunk_io = vm->frames[handler.frame].chunk;
    *ip_io = handler.target;
    return true;
}

static LzStatus try_begin_op(LzVM *vm, const LzChunk *chunk, size_t *ip) {
    uint16_t distance;
    if (!read_u16(chunk, ip, &distance)) {
        return malformed(vm, "truncated operand");
    }
    if (*ip + distance > chunk->count) {
        return malformed(vm, "jump out of range");
    }
    if (vm->handler_count >= LZ_HANDLERS_MAX) {
        return lz_vm_set_error(vm, LZ_ERR_STACK_OVERFLOW, "stack overflow: too many open try blocks");
    }
    LzHandler *handler = &vm->handlers[vm->handler_count++];
    handler->frame = vm->frame_count - 1;
    handler->target = *ip + distance;
    handler->stack_height = vm->stack_count;
    return LZ_OK;
}

static LzStatus throw_op(LzVM *vm) {
    if (vm->stack_count == 0) {
        return malformed(vm, "stack underflow");
    }
    LzValue *top = &vm->stack[vm->stack_count - 1];
    if (!lz_value_is_error(top)) {
        return lz_vm_set_error(vm, LZ_ERR_TYPE,
                               "only an error can be thrown (write RuntimeError(\"oops\"))");
    }
    LzError *error = (LzError *)top->as.object;
    LzValue thrown = *top; /* the stack's reference moves to vm->thrown */
    vm->stack_count--;
    if (vm->thrown.type != LZ_VAL_NIL) {
        LzValue old = vm->thrown;
        vm->thrown = lz_value_nil();
        lz_value_release(&vm->heap, &old);
    }
    vm->thrown = thrown;
    vm->error_suggestions.count = 0;
    vm->error_help[0] = '\0';
    (void)snprintf(vm->error, sizeof vm->error, "%s: %.*s",
                   error->kind_name->data, (int)error->message->length,
                   error->message->data);
    vm->error_kind = error->kind;
    return LZ_ERR_RUNTIME;
}

static LzStatus error_is_op(LzVM *vm, uint8_t kind) {
    if (vm->stack_count == 0 || !lz_value_is_error(&vm->stack[vm->stack_count - 1])) {
        return malformed(vm, "a catch needs an error value");
    }
    const LzError *error = (const LzError *)vm->stack[vm->stack_count - 1].as.object;
    bool is = lz_vm_error_kind_is(vm, error->kind, kind);
    pop(vm);
    return push(vm, lz_value_bool(is));
}

static uint64_t current_owner(const LzVM *vm) {
    const LzCallFrame *frame = &vm->frames[vm->frame_count - 1];
    return frame->closure != NULL ? frame->closure->function->owner_id : 0;
}

/* GET_FIELD_KEEP and the object case of GET_FIELD. */
static LzStatus member_get_op(LzVM *vm, const LzString *name, bool keep, bool for_write) {
    LzValue target = vm->stack[vm->stack_count - 1];
    LzValue found = lz_value_nil();
    lz_value_retain(&target); /* a getter runs script code that may use the stack */
    LzStatus status = lz_vm_member_get(vm, current_owner(vm), &target, name, &found, for_write);
    if (status != LZ_OK) {
        lz_value_release(&vm->heap, &target);
        return status;
    }
    if (keep) {
        if (vm->stack_count >= LZ_STACK_MAX) {
            lz_value_release(&vm->heap, &found);
            lz_value_release(&vm->heap, &target);
            return lz_vm_set_error(vm, LZ_ERR_STACK_OVERFLOW, "stack overflow");
        }
        vm->stack[vm->stack_count++] = found;
    } else {
        LzValue old = vm->stack[vm->stack_count - 1];
        vm->stack[vm->stack_count - 1] = found;
        lz_value_release(&vm->heap, &old);
    }
    lz_value_release(&vm->heap, &target);
    return LZ_OK;
}

static LzStatus set_field_op(LzVM *vm, const LzChunk *chunk, size_t *ip) {
    LzString *name = NULL;
    LzStatus status = read_name(vm, chunk, ip, &name);
    if (status != LZ_OK) {
        return status;
    }
    if (vm->stack_count < 2) {
        return malformed(vm, "stack underflow");
    }
    LzValue value = vm->stack[vm->stack_count - 1];
    LzValue target = vm->stack[vm->stack_count - 2];
    if (!lz_value_is_instance(&target) && !lz_value_is_class(&target)) {
        return lz_vm_set_error(vm, LZ_ERR_TYPE, "cannot assign to field '%s' of %s", name->data,
                               lz_value_kind_name(&target));
    }
    status = lz_vm_member_set(vm, current_owner(vm), &target, name, &value);
    if (status != LZ_OK) {
        return status;
    }
    pop(vm);
    pop(vm);
    return LZ_OK;
}

static LzStatus method_lookup_op(LzVM *vm, const LzChunk *chunk, size_t *ip) {
    LzString *name = NULL;
    LzStatus status = read_name(vm, chunk, ip, &name);
    if (status != LZ_OK) {
        return status;
    }
    if (*ip >= chunk->count) {
        return malformed(vm, "truncated operand");
    }
    int mode = chunk->code[(*ip)++];
    if (vm->stack_count < 2) {
        return malformed(vm, "stack underflow");
    }
    return lz_vm_method_lookup(vm, current_owner(vm), name, mode, vm->stack_count - 2);
}

static LzStatus class_init_op(LzVM *vm, const LzChunk *chunk, size_t *ip) {
    const LzValue *constant = NULL;
    LzStatus status = read_constant(vm, chunk, ip, &constant);
    if (status != LZ_OK) {
        return status;
    }
    if (!lz_value_is_class(constant)) {
        return malformed(vm, "class expected");
    }
    LzClass *cls = lz_value_as_class(constant);
    status = lz_class_finish(vm, cls);
    if (status != LZ_OK) {
        return status;
    }
    return lz_class_run_consts(vm, cls);
}

static LzStatus super_op(LzVM *vm, const LzChunk *chunk, size_t *ip) {
    LzString *name = NULL;
    LzStatus status = read_name(vm, chunk, ip, &name);
    if (status != LZ_OK) {
        return status;
    }
    if (*ip >= chunk->count) {
        return malformed(vm, "truncated operand");
    }
    int mode = chunk->code[(*ip)++];
    size_t need = mode == 0 ? 2 : 1;
    if (vm->stack_count < need) {
        return malformed(vm, "stack underflow");
    }
    return lz_vm_super_lookup(vm, current_owner(vm), name, mode, vm->stack_count - need);
}

/* The head `class A : B, C` after the name is bound: `B` is on the stack. */
static LzStatus class_link_op(LzVM *vm, const LzChunk *chunk, size_t *ip) {
    const LzValue *constant = NULL;
    LzStatus status = read_constant(vm, chunk, ip, &constant);
    if (status != LZ_OK) {
        return status;
    }
    if (!lz_value_is_class(constant)) {
        return malformed(vm, "class expected");
    }
    LzString *parent_name = NULL;
    status = read_name(vm, chunk, ip, &parent_name);
    if (status != LZ_OK) {
        return status;
    }
    if (*ip >= chunk->count) {
        return malformed(vm, "truncated operand");
    }
    uint8_t role = chunk->code[(*ip)++];
    if (vm->stack_count == 0) {
        return malformed(vm, "stack underflow");
    }
    LzClass *cls = lz_value_as_class(constant);
    const LzValue *top = &vm->stack[vm->stack_count - 1];
    if (lz_value_is_class(top)) {
        LzClass *other = lz_value_as_class(top);
        if (cls->is_trait) {
            status = other->is_trait ? lz_class_add_trait(vm, cls, other)
                                     : lz_vm_set_error(vm, LZ_ERR_RUNTIME, "a trait can only extend other traits");
        } else if (other->is_trait) {
            status = lz_class_add_trait(vm, cls, other);
        } else if (role != 0) {
            status = lz_vm_set_error(vm, LZ_ERR_RUNTIME,
                                     "a class can have only one parent class, and it must come first");
        } else {
            status = lz_class_link(vm, cls, other);
        }
    } else if (top->type == LZ_VAL_OBJECT && top->as.object->type == LZ_OBJ_NATIVE &&
               lz_native_error_kind((const LzNative *)top->as.object, NULL)) {
        status = lz_vm_set_error(vm, LZ_ERR_RUNTIME, "inheriting from an error kind is not supported yet");
    } else {
        status = lz_vm_set_error(vm, LZ_ERR_RUNTIME, "'%s' is not a class or a trait", parent_name->data);
    }
    pop(vm);
    return status;
}

/* `e.message`, `e.kind`, `e.trace`, `e.cause`, `e.data`: the fields of an error, read only. */
static LzStatus get_field_op(LzVM *vm, const LzChunk *chunk, size_t *ip, bool write_check) {
    LzString *name = NULL;
    LzStatus status = read_name(vm, chunk, ip, &name);
    if (status != LZ_OK) {
        return status;
    }
    if (vm->stack_count == 0) {
        return malformed(vm, "stack underflow");
    }
    const LzValue *top = &vm->stack[vm->stack_count - 1];
    if (lz_value_is_instance(top) || lz_value_is_class(top)) {
        return member_get_op(vm, name, false, write_check);
    }
    if (!lz_value_is_error(top)) {
        return lz_vm_set_error(vm, LZ_ERR_TYPE, "%s has no field '%s'",
                               lz_value_kind_name(top), name->data);
    }
    LzError *error = (LzError *)top->as.object;
    LzValue field;
    LzArray *empty = NULL;
    if (strcmp(name->data, "message") == 0) {
        field = lz_value_object(&error->message->base);
    } else if (strcmp(name->data, "kind") == 0) {
        field = lz_value_object(&error->kind_name->base); /* interned when the error was made */
    } else if (strcmp(name->data, "cause") == 0) {
        field = error->cause;
    } else if (strcmp(name->data, "data") == 0) {
        field = error->data;
    } else if (strcmp(name->data, "trace") == 0) {
        if (error->trace == NULL) {
            empty = lz_array_new(&vm->heap, 0);
            if (empty == NULL) {
                return lz_vm_out_of_memory(vm);
            }
            error->trace = empty;
        }
        field = lz_value_object(&error->trace->base);
    } else {
        return lz_vm_set_error(vm, LZ_ERR_TYPE, "error has no field '%s'", name->data);
    }
    lz_value_retain(&field);
    LzValue holder = *top; /* the error: released once the field is safe on the stack */
    vm->stack[vm->stack_count - 1] = field;
    lz_value_release(&vm->heap, &holder);
    return LZ_OK;
}

static LzStatus range_last_op(LzVM *vm, bool inclusive) {
    if (vm->stack_count < 3) {
        return malformed(vm, "stack underflow");
    }
    const LzValue *counter = &vm->stack[vm->stack_count - 3];
    const LzValue *limit = &vm->stack[vm->stack_count - 2];
    const LzValue *step = &vm->stack[vm->stack_count - 1];
    if (!range_bounds_int(vm, counter, limit, step)) {
        return LZ_ERR_TYPE; /* already reported */
    }
    int64_t start = counter->as.integer;
    int64_t end = limit->as.integer;
    int64_t by = step->as.integer;
    if (by == 0) {
        return lz_vm_set_error(vm, LZ_ERR_RUNTIME, "range step cannot be 0");
    }
    /* The distance is taken modulo 2^64, which is exact while the range runs the right way. */
    uint64_t magnitude = by > 0 ? (uint64_t)by : (uint64_t)0 - (uint64_t)by;
    int64_t last;
    if (by > 0) {
        uint64_t distance = (uint64_t)end - (uint64_t)start;
        last = inclusive ? end - (int64_t)(distance % magnitude)
                         : end - 1 - (int64_t)((distance - 1) % magnitude);
    } else {
        uint64_t distance = (uint64_t)start - (uint64_t)end;
        last = inclusive ? end + (int64_t)(distance % magnitude)
                         : end + 1 + (int64_t)((distance - 1) % magnitude);
    }
    pop(vm);
    pop(vm);
    pop(vm);
    return push(vm, lz_value_int(last));
}

static LzStatus range_next_op(LzVM *vm) {
    if (vm->stack_count < 2) {
        return malformed(vm, "stack underflow");
    }
    const LzValue *counter = &vm->stack[vm->stack_count - 2];
    const LzValue *step = &vm->stack[vm->stack_count - 1];
    if (counter->type != LZ_VAL_INT || step->type != LZ_VAL_INT) {
        (void)lz_vm_set_error(vm, LZ_ERR_TYPE, "range bounds must be ints, got %s",
                              lz_kind_name(lz_kind_of(counter->type != LZ_VAL_INT ? counter : step)));
        return LZ_ERR_TYPE;
    }
    int64_t start = counter->as.integer;
    int64_t by = step->as.integer;
    /*
     * Safe without a check: the loop steps only while the counter is before the last
     * value, and the last value is one the sequence reaches, so the sum never leaves
     * the range.
     */
    int64_t next = start + by;
    pop(vm);
    pop(vm);
    return push(vm, lz_value_int(next));
}

/* The slot is validated by the caller: this only closes the upvalue open at it. */
static LzStatus close_upvalue_keep_op(LzVM *vm, uint16_t slot) {
    if (slot >= vm->stack_count) {
        return malformed(vm, "local slot out of range");
    }
    lz_vm_close_upvalues(vm, slot);
    lz_skink_run(vm);
    return LZ_OK;
}

/* [array][index] -> [array][index][item]: the array and the index stay for OP_SET_INDEX. */
static LzStatus index_keep_op(LzVM *vm) {
    if (vm->stack_count < 2) {
        return malformed(vm, "stack underflow");
    }
    const LzValue *container = &vm->stack[vm->stack_count - 2];
    if (lz_value_is_dict(container)) {
        /* `d[k] op= v`: the read keeps the dict and the key for OP_SET_INDEX below. */
        const LzDict *dict = lz_value_as_dict(container);
        const LzValue *key = &vm->stack[vm->stack_count - 1];
        LzStatus status = lz_dict_own_key(vm, key);
        if (status != LZ_OK) {
            return status;
        }
        const LzValue *stored = lz_dict_get(dict, key);
        if (stored == NULL) {
            return lz_vm_dict_key_error(vm, dict, key, " (use d.get(k, 0) + v)");
        }
        return push(vm, *stored);
    }
    if (!lz_value_is_array(container)) {
        return not_indexable(vm, container);
    }
    const LzArray *array = lz_value_as_array(container);
    size_t position = 0;
    LzStatus status = lz_array_resolve_index(vm, &vm->stack[vm->stack_count - 1], array->count,
                                             &position);
    if (status != LZ_OK) {
        return status;
    }
    return push(vm, array->items[position]);
}

static LzStatus set_index_op(LzVM *vm) {
    if (vm->stack_count < 3) {
        return malformed(vm, "stack underflow");
    }
    const LzValue *container = &vm->stack[vm->stack_count - 3];
    if (lz_value_is_dict(container)) {
        LzStatus status = lz_dict_set(vm, lz_value_as_dict(container),
                                      &vm->stack[vm->stack_count - 2],
                                      &vm->stack[vm->stack_count - 1]);
        if (status != LZ_OK) {
            return status;
        }
        pop(vm);
        pop(vm);
        pop(vm);
        return LZ_OK;
    }
    if (!lz_value_is_array(container)) {
        return not_indexable(vm, container);
    }
    LzStatus status = lz_array_set(vm, lz_value_as_array(container),
                                   &vm->stack[vm->stack_count - 2], &vm->stack[vm->stack_count - 1]);
    if (status != LZ_OK) {
        return status;
    }
    pop(vm);
    pop(vm);
    pop(vm);
    return LZ_OK;
}

static LzStatus slice_op(LzVM *vm) {
    if (vm->stack_count < 3) {
        return malformed(vm, "stack underflow");
    }
    const LzValue *container = &vm->stack[vm->stack_count - 3];
    if (!lz_value_is_array(container)) {
        return not_indexable(vm, container);
    }
    LzValue result = lz_value_nil();
    LzStatus status = lz_array_slice(vm, lz_value_as_array(container),
                                     &vm->stack[vm->stack_count - 2],
                                     &vm->stack[vm->stack_count - 1], &result);
    if (status != LZ_OK) {
        return status;
    }
    pop(vm);
    pop(vm);
    pop(vm);
    vm->stack[vm->stack_count++] = result;
    return LZ_OK;
}

static LzStatus array_extend(LzVM *vm) {
    if (vm->stack_count < 2) {
        return malformed(vm, "stack underflow");
    }
    const LzValue *target = &vm->stack[vm->stack_count - 2];
    const LzValue *source = &vm->stack[vm->stack_count - 1];
    if (!lz_value_is_array(target)) {
        return malformed(vm, "array expected");
    }
    if (!lz_value_is_array(source)) {
        return lz_vm_set_error(vm, LZ_ERR_RUNTIME, "cannot spread a value of type %s",
                               lz_value_kind_name(source));
    }
    const LzArray *items = lz_value_as_array(source);
    LzStatus status = lz_vm_charge_fuel(vm, lz_array_cost(items->count));
    for (size_t i = 0; status == LZ_OK && i < items->count; i++) {
        status = lz_array_push(vm, lz_value_as_array(target), &items->items[i]);
    }
    if (status == LZ_OK) {
        pop(vm);
    }
    return status;
}

static LzStatus binary_op(LzVM *vm, LzOpCode op) {
    if (vm->stack_count < 2) {
        return malformed(vm, "stack underflow");
    }

    LzValue result = lz_value_nil();
    const LzValue *left = &vm->stack[vm->stack_count - 2];
    const LzValue *right = &vm->stack[vm->stack_count - 1];
    bool is_order = op == LZ_OP_LESS || op == LZ_OP_GREATER || op == LZ_OP_LESS_EQUAL ||
                    op == LZ_OP_GREATER_EQUAL;
    LzStatus status = is_order ? lz_compare_order(vm, op, left, right, &result)
                               : lz_arith_binary(vm, op, left, right, &result);
    if (status != LZ_OK) {
        return status;
    }
    pop(vm);
    pop(vm);
    vm->stack[vm->stack_count++] = result;
    return LZ_OK;
}

static LzStatus equal(LzVM *vm) {
    if (vm->stack_count < 2) {
        return malformed(vm, "stack underflow");
    }

    const LzValue *left = &vm->stack[vm->stack_count - 2];
    const LzValue *right = &vm->stack[vm->stack_count - 1];

    bool same = false;
    LzStatus status = lz_vm_values_equal(vm, left, right, &same);
    if (status != LZ_OK) {
        return status;
    }
    pop(vm);
    pop(vm);
    vm->stack[vm->stack_count++] = lz_value_bool(same);
    return LZ_OK;
}

static LzStatus not_op(LzVM *vm) {
    if (vm->stack_count < 1) {
        return malformed(vm, "stack underflow");
    }

    bool falsy = !lz_value_is_truthy(&vm->stack[vm->stack_count - 1]);
    pop(vm);
    vm->stack[vm->stack_count++] = lz_value_bool(falsy);
    return LZ_OK;
}

static LzStatus negate(LzVM *vm) {
    if (vm->stack_count < 1) {
        return malformed(vm, "stack underflow");
    }

    LzValue result = lz_value_nil();
    LzStatus status = lz_arith_negate(vm, &vm->stack[vm->stack_count - 1], &result);
    if (status != LZ_OK) {
        return status;
    }
    pop(vm);
    vm->stack[vm->stack_count++] = result;
    return LZ_OK;
}

static LzStatus bit_not(LzVM *vm) {
    if (vm->stack_count < 1) {
        return malformed(vm, "stack underflow");
    }

    LzValue result = lz_value_nil();
    LzStatus status = lz_arith_bit_not(vm, &vm->stack[vm->stack_count - 1], &result);
    if (status != LZ_OK) {
        return status;
    }
    pop(vm);
    vm->stack[vm->stack_count++] = result;
    return LZ_OK;
}

/*
 * Runs the one instruction at byte offset *ip_io and advances it past the
 * instruction. It does no accounting (fuel, vm->instructions): the loop that
 * calls it does that, so a faster engine can spend fuel its own way and still
 * reuse every semantic.
 *
 * OP_RETURN is reported by setting *ip_io to SIZE_MAX, which the loop already
 * has to treat as "no more instructions": the hot path pays nothing for it.
 * Marked always_inline so the reference loop below keeps its speed.
 */
#if defined(__GNUC__) || defined(__clang__)
#define LZ_FORCE_INLINE static inline __attribute__((always_inline))
#else
#define LZ_FORCE_INLINE static inline
#endif

LZ_FORCE_INLINE LzStatus exec_one(LzVM *vm, const LzChunk **chunk_io, size_t *ip_io,
                                  bool frames_ok) {
    const LzChunk *chunk = *chunk_io;
    size_t ip = *ip_io;
    LzStatus status = LZ_OK;
    if (ip >= chunk->count) {
        return malformed(vm, "chunk ended without OP_RETURN");
    }

    const size_t start = ip;
    uint8_t opcode = chunk->code[ip++];
    switch (opcode) {
    case LZ_OP_CONSTANT: {
        const LzValue *constant = NULL;
        status = read_constant(vm, chunk, &ip, &constant);
        if (status == LZ_OK) {
            status = push(vm, *constant);
        }
        break;
    }
    case LZ_OP_NIL:
        status = push(vm, lz_value_nil());
        break;
    case LZ_OP_TRUE:
        status = push(vm, lz_value_bool(true));
        break;
    case LZ_OP_FALSE:
        status = push(vm, lz_value_bool(false));
        break;
    case LZ_OP_POP:
        if (vm->stack_count == 0) {
            status = malformed(vm, "stack underflow");
        } else {
            pop(vm);
        }
        break;
    case LZ_OP_GET_GLOBAL: {
        LzString *name;
        status = read_name(vm, chunk, &ip, &name);
        if (status != LZ_OK) {
            break;
        }
        const LzValue *value = lz_vm_lookup(vm, name);
        if (value == NULL) {
            status = lz_vm_set_error(vm, LZ_ERR_RUNTIME, "undefined variable '%s'", name->data);
        } else {
            status = push(vm, *value);
        }
        break;
    }
    case LZ_OP_DEFINE_GLOBAL: {
        LzString *name;
        status = read_name(vm, chunk, &ip, &name);
        if (status != LZ_OK) {
            break;
        }
        if (vm->stack_count == 0) {
            status = malformed(vm, "stack underflow");
        } else if (!lz_vm_define_global(vm, name, vm->stack[vm->stack_count - 1])) {
            status = lz_vm_out_of_memory(vm);
        } else {
            pop(vm);
        }
        break;
    }
    case LZ_OP_CALL: {
        if (ip >= chunk->count) {
            status = malformed(vm, "truncated operand");
            break;
        }
        size_t argc = chunk->code[ip++];
        if (!frames_ok && vm->stack_count > argc && lz_value_is_closure(&vm->stack[vm->stack_count - argc - 1])) {
            status = malformed(vm, "a single step cannot enter a function");
            break;
        }
        bool entered = false;
        status = lz_vm_call(vm, argc, ip, (uint32_t)start, &entered);
        if (entered) {
            *chunk_io = chunk = vm->frames[vm->frame_count - 1].chunk;
            ip = 0;
        }
        break;
    }
    case LZ_OP_CALL_EX: {
        uint16_t names;
        if (ip >= chunk->count) {
            status = malformed(vm, "truncated operand");
            break;
        }
        size_t named = chunk->code[ip++];
        if (!read_u16(chunk, &ip, &names)) {
            status = malformed(vm, "truncated operand");
        } else if (!frames_ok) {
            status = malformed(vm, "a single step cannot enter a function");
        } else {
            bool entered = false;
            status = lz_vm_call_ex(vm, chunk, named, names, ip, (uint32_t)start, &entered);
            if (entered) {
                *chunk_io = chunk = vm->frames[vm->frame_count - 1].chunk;
                ip = 0;
            }
        }
        break;
    }
    case LZ_OP_ADD:
    case LZ_OP_SUBTRACT:
    case LZ_OP_MULTIPLY:
    case LZ_OP_DIVIDE:
    case LZ_OP_FLOOR_DIVIDE:
    case LZ_OP_MODULO:
    case LZ_OP_LESS:
    case LZ_OP_GREATER:
    case LZ_OP_LESS_EQUAL:
    case LZ_OP_GREATER_EQUAL:
    case LZ_OP_BIT_AND:
    case LZ_OP_BIT_OR:
    case LZ_OP_BIT_XOR:
    case LZ_OP_SHL:
    case LZ_OP_SHR:
        status = binary_op(vm, (LzOpCode)opcode);
        break;
    case LZ_OP_GET_LOCAL: {
        uint16_t slot;
        if (!read_u16(chunk, &ip, &slot)) {
            status = malformed(vm, "truncated operand");
        } else if (vm->frames[vm->frame_count - 1].base + slot >= vm->stack_count) {
            status = malformed(vm, "local slot out of range");
        } else {
            status = push(vm, vm->stack[vm->frames[vm->frame_count - 1].base + slot]);
        }
        break;
    }
    case LZ_OP_SET_LOCAL: {
        uint16_t slot;
        if (!read_u16(chunk, &ip, &slot)) {
            status = malformed(vm, "truncated operand");
        } else if (vm->stack_count == 0 ||
                   vm->frames[vm->frame_count - 1].base + slot >= vm->stack_count - 1) {
            status = malformed(vm, "local slot out of range");
        } else {
            size_t index = vm->frames[vm->frame_count - 1].base + slot;
            LzValue moved = vm->stack[--vm->stack_count];
            lz_value_release(&vm->heap, &vm->stack[index]);
            vm->stack[index] = moved;
        }
        break;
    }
    case LZ_OP_SET_GLOBAL: {
        LzString *name;
        status = read_name(vm, chunk, &ip, &name);
        if (status != LZ_OK) {
            break;
        }
        if (vm->stack_count == 0) {
            status = malformed(vm, "stack underflow");
        } else if (lz_table_get(&vm->globals, name) == NULL) {
            status = lz_vm_set_error(vm, LZ_ERR_RUNTIME, "undefined variable '%s'", name->data);
        } else if (!lz_vm_define_global(vm, name, vm->stack[vm->stack_count - 1])) {
            status = lz_vm_out_of_memory(vm);
        } else {
            pop(vm);
        }
        break;
    }
    case LZ_OP_JUMP:
    case LZ_OP_JUMP_IF_FALSE:
    case LZ_OP_JUMP_IF_TRUE: {
        uint16_t distance;
        if (!read_u16(chunk, &ip, &distance)) {
            status = malformed(vm, "truncated operand");
        } else if (ip + distance > chunk->count) {
            status = malformed(vm, "jump out of range");
        } else if (opcode == LZ_OP_JUMP) {
            ip += distance;
        } else if (vm->stack_count == 0) {
            status = malformed(vm, "stack underflow");
        } else if (lz_value_is_truthy(&vm->stack[vm->stack_count - 1]) ==
                   (opcode == LZ_OP_JUMP_IF_TRUE)) {
            ip += distance;
        }
        break;
    }
    case LZ_OP_LOOP: {
        uint16_t distance;
        if (!read_u16(chunk, &ip, &distance)) {
            status = malformed(vm, "truncated operand");
        } else if (distance > ip) {
            status = malformed(vm, "jump out of range");
        } else {
            ip -= distance;
        }
        break;
    }
    case LZ_OP_EQUAL:
        status = equal(vm);
        break;
    case LZ_OP_NOT:
        status = not_op(vm);
        break;
    case LZ_OP_NEGATE:
        status = negate(vm);
        break;
    case LZ_OP_BIT_NOT:
        status = bit_not(vm);
        break;
    case LZ_OP_CHECK_TYPE: {
        uint16_t index;
        if (!read_u16(chunk, &ip, &index)) {
            status = malformed(vm, "truncated operand");
        } else if (index >= chunk->type_count) {
            status = malformed(vm, "type index out of range");
        } else if (vm->stack_count == 0) {
            status = malformed(vm, "stack underflow");
        } else {
            status = check_value(vm, &chunk->types[index], &vm->stack[vm->stack_count - 1],
                                 CHECK_VALUE, 0);
        }
        break;
    }
    case LZ_OP_IS: {
        uint16_t index;
        if (!read_u16(chunk, &ip, &index)) {
            status = malformed(vm, "truncated operand");
        } else if (index >= chunk->type_count) {
            status = malformed(vm, "type index out of range");
        } else if (vm->stack_count == 0) {
            status = malformed(vm, "stack underflow");
        } else {
            LzValue *top = &vm->stack[vm->stack_count - 1];
            bool matches = lz_type_matches(&chunk->types[index], top);
            lz_value_release(&vm->heap, top);
            *top = lz_value_bool(matches);
        }
        break;
    }
    case LZ_OP_CHECK_PARAM: {
        uint16_t slot;
        uint16_t index;
        if (!read_u16(chunk, &ip, &slot) || !read_u16(chunk, &ip, &index)) {
            status = malformed(vm, "truncated operand");
        } else if (index >= chunk->type_count) {
            status = malformed(vm, "type index out of range");
        } else if (vm->frames[vm->frame_count - 1].base + slot >= vm->stack_count) {
            status = malformed(vm, "local slot out of range");
        } else {
            status = check_value(vm, &chunk->types[index],
                                 &vm->stack[vm->frames[vm->frame_count - 1].base + slot],
                                 CHECK_PARAM, slot);
        }
        break;
    }
    case LZ_OP_CHECK_RETURN: {
        uint16_t index;
        if (!read_u16(chunk, &ip, &index)) {
            status = malformed(vm, "truncated operand");
        } else if (index >= chunk->type_count) {
            status = malformed(vm, "type index out of range");
        } else if (vm->stack_count == 0) {
            status = malformed(vm, "stack underflow");
        } else {
            status = check_value(vm, &chunk->types[index], &vm->stack[vm->stack_count - 1],
                                 CHECK_RETURN, 0);
        }
        break;
    }
    case LZ_OP_JUMP_IF_SUPPLIED: {
        uint16_t parameter;
        uint16_t distance;
        if (!read_u16(chunk, &ip, &parameter) || !read_u16(chunk, &ip, &distance)) {
            status = malformed(vm, "truncated operand");
        } else if (ip + distance > chunk->count) {
            status = malformed(vm, "jump out of range");
        } else if (parameter >= 64 ||
                   ((vm->frames[vm->frame_count - 1].supplied >> parameter) & 1u) != 0) {
            ip += distance;
        }
        break;
    }
    case LZ_OP_CLOSURE: {
        const LzValue *constant = NULL;
        status = read_constant(vm, chunk, &ip, &constant);
        if (status != LZ_OK) {
            break;
        }
        if (constant->type != LZ_VAL_OBJECT || constant->as.object->type != LZ_OBJ_FUNCTION) {
            status = malformed(vm, "closure operand is not a function");
        } else {
            status = lz_vm_make_closure(vm, (LzFunction *)constant->as.object);
        }
        break;
    }
    case LZ_OP_GET_UPVALUE:
    case LZ_OP_SET_UPVALUE: {
        uint16_t index;
        const LzClosure *closure = vm->frames[vm->frame_count - 1].closure;
        if (!read_u16(chunk, &ip, &index)) {
            status = malformed(vm, "truncated operand");
        } else if (closure == NULL || index >= closure->upvalue_count) {
            status = malformed(vm, "upvalue index out of range");
        } else if (opcode == LZ_OP_GET_UPVALUE) {
            status = push(vm, *closure->upvalues[index]->location);
        } else if (vm->stack_count == 0) {
            status = malformed(vm, "stack underflow");
        } else {
            LzUpvalue *upvalue = closure->upvalues[index];
            LzValue *location = upvalue->location;
            LzValue moved = vm->stack[--vm->stack_count];
            LzValue old = *location;
            *location = moved;
            lz_value_release(&vm->heap, &old);
            if (moved.type == LZ_VAL_OBJECT) {
                /* The GE interpreter delegates this instruction to here, and both JIT back-ends
                 * send every object store here too: they inline only what cannot close a cycle. */
                lz_skink_note_store(vm, upvalue);
            }
        }
        break;
    }
    case LZ_OP_CLOSE_UPVALUE:
        if (vm->stack_count == 0) {
            status = malformed(vm, "stack underflow");
        } else {
            lz_vm_close_upvalues(vm, vm->stack_count - 1);
            pop(vm);
            lz_skink_run(vm);
        }
        break;
    case LZ_OP_GET_SELF: {
        const LzClosure *closure = vm->frames[vm->frame_count - 1].closure;
        if (closure == NULL) {
            status = malformed(vm, "no running function");
        } else {
            status = push(vm, lz_value_object((LzObject *)&closure->base));
        }
        break;
    }
    case LZ_OP_SWAP:
        if (vm->stack_count < 2) {
            status = malformed(vm, "stack underflow");
        } else {
            LzValue top = vm->stack[vm->stack_count - 1];
            vm->stack[vm->stack_count - 1] = vm->stack[vm->stack_count - 2];
            vm->stack[vm->stack_count - 2] = top;
        }
        break;
    case LZ_OP_ARRAY_EXTEND:
        status = array_extend(vm);
        break;
    case LZ_OP_ARRAY_NEW:
        status = array_new(vm);
        break;
    case LZ_OP_ARRAY_PUSH:
        status = array_push(vm);
        break;
    case LZ_OP_INDEX:
        status = index_op(vm);
        break;
    case LZ_OP_SET_INDEX:
        status = set_index_op(vm);
        break;
    case LZ_OP_INDEX_KEEP:
        status = index_keep_op(vm);
        break;
    case LZ_OP_ITER_LENGTH:
        status = iter_length_op(vm);
        break;
    case LZ_OP_ITER_NEXT:
        status = iter_next_op(vm);
        break;
    case LZ_OP_ITER_PAIR: {
        status = iter_pair_op(vm);
        break;
    }
    case LZ_OP_ITER_CHECK:
        status = iter_check_op(vm);
        break;
    case LZ_OP_DICT: {
        uint16_t count;
        if (!read_u16(chunk, &ip, &count)) {
            status = malformed(vm, "truncated operand");
        } else {
            status = dict_new(vm, count);
        }
        break;
    }
    case LZ_OP_RANGE_TEST: {
        bool inclusive = ip < chunk->count && chunk->code[ip++] != 0;
        status = range_test_op(vm, inclusive);
        break;
    }
    case LZ_OP_RANGE_LAST: {
        bool inclusive = ip < chunk->count && chunk->code[ip++] != 0;
        status = range_last_op(vm, inclusive);
        break;
    }
    case LZ_OP_RANGE_NEXT:
        status = range_next_op(vm);
        break;
    case LZ_OP_IN:
        status = in_op(vm);
        break;
    case LZ_OP_IN_RANGE: {
        uint8_t flags = ip < chunk->count ? chunk->code[ip++] : 0;
        status = in_range_op(vm, (flags & 1) != 0, (flags & 2) != 0);
        break;
    }
    case LZ_OP_NIL_TEST:
        status = nil_test_op(vm);
        break;
    case LZ_OP_TRY_BEGIN:
        status = try_begin_op(vm, chunk, &ip);
        break;
    case LZ_OP_TRY_END:
        if (vm->handler_count > 0) {
            vm->handler_count--;
        }
        break;
    case LZ_OP_THROW:
        status = throw_op(vm);
        break;
    case LZ_OP_ERROR_IS: {
        uint8_t kind = ip < chunk->count ? chunk->code[ip++] : 0;
        status = error_is_op(vm, kind);
        break;
    }
    case LZ_OP_GET_FIELD:
        status = get_field_op(vm, chunk, &ip, false);
        break;
    case LZ_OP_GET_FIELD_W:
        status = get_field_op(vm, chunk, &ip, true);
        break;
    case LZ_OP_GET_FIELD_KEEP: {
        LzString *name = NULL;
        status = read_name(vm, chunk, &ip, &name);
        if (status == LZ_OK) {
            if (vm->stack_count == 0) {
                status = malformed(vm, "stack underflow");
            } else if (!lz_value_is_instance(&vm->stack[vm->stack_count - 1]) &&
                       !lz_value_is_class(&vm->stack[vm->stack_count - 1])) {
                status = lz_vm_set_error(vm, LZ_ERR_TYPE, "%s has no field '%s'",
                                         lz_value_kind_name(&vm->stack[vm->stack_count - 1]), name->data);
            } else {
                status = member_get_op(vm, name, true, false);
            }
        }
        break;
    }
    case LZ_OP_SET_FIELD:
        status = set_field_op(vm, chunk, &ip);
        break;
    case LZ_OP_METHOD_LOOKUP:
        status = method_lookup_op(vm, chunk, &ip);
        break;
    case LZ_OP_CLASS_INIT:
        status = class_init_op(vm, chunk, &ip);
        break;
    case LZ_OP_CLASS_LINK:
        status = class_link_op(vm, chunk, &ip);
        break;
    case LZ_OP_SUPER:
        status = super_op(vm, chunk, &ip);
        break;
    case LZ_OP_DUP:
        if (vm->stack_count == 0) {
            status = malformed(vm, "stack underflow");
        } else {
            status = push(vm, vm->stack[vm->stack_count - 1]);
        }
        break;
    case LZ_OP_CLOSE_UPVALUE_KEEP: {
        uint16_t slot;
        if (!read_u16(chunk, &ip, &slot)) {
            status = malformed(vm, "truncated operand");
        } else {
            status = close_upvalue_keep_op(vm, slot);
        }
        break;
    }
    case LZ_OP_SLICE:
        status = slice_op(vm);
        break;
    case LZ_OP_RETURN:
        if (vm->frame_count <= 1) {
            ip = SIZE_MAX;
        } else if (!frames_ok) {
            status = malformed(vm, "a single step cannot leave a function");
        } else {
            status = lz_vm_return(vm);
            if (status == LZ_OK) {
                const LzCallFrame *caller = &vm->frames[vm->frame_count - 1];
                *chunk_io = chunk = caller->chunk;
                ip = caller->ip;
            }
        }
        break;
    default:
        status = malformed(vm, "unknown opcode");
        break;
    }

    *ip_io = ip;
    return status;
}

LzStatus lz_vm_exec_one(LzVM *vm, const LzChunk *chunk, size_t *ip, bool *halted) {
    size_t next = *ip;
    const LzChunk *current = chunk;
    LzStatus status = exec_one(vm, &current, &next, false);
    *halted = next == SIZE_MAX;
    *ip = *halted ? *ip + 1 : next;
    return status;
}

static LzStatus execute(LzVM *vm, const LzChunk *main_chunk, size_t *failed_at, void *context) {
    (void)context;
    const LzChunk *chunk = main_chunk;
    size_t ip = 0;

    for (;;) {
        *failed_at = ip;
        if (ip >= chunk->count) {
            return ip == SIZE_MAX ? LZ_OK : malformed(vm, "chunk ended without OP_RETURN");
        }

        LzStatus status = lz_vm_charge_fuel(vm, 1);
        if (status != LZ_OK) {
            return status;
        }
        vm->instructions++;

        size_t at = ip;
        status = exec_one(vm, &chunk, &ip, true);
        if (status != LZ_OK) {
            if (unwind_to_handler(vm, &status, 0, at, &chunk, &ip)) {
                continue;
            }
            return status;
        }
    }
}

LzStatus lz_vm_run_until(LzVM *vm, size_t depth, size_t *failed_at) {
    const LzCallFrame *top = &vm->frames[vm->frame_count - 1];
    const LzChunk *chunk = top->chunk;
    size_t ip = top->ip;

    while (vm->frame_count > depth) {
        *failed_at = ip;
        if (ip >= chunk->count) {
            return malformed(vm, "chunk ended without OP_RETURN");
        }
        LzStatus status = lz_vm_charge_fuel(vm, 1);
        if (status != LZ_OK) {
            return status;
        }
        vm->instructions++;
        size_t at = ip;
        status = exec_one(vm, &chunk, &ip, true);
        if (status != LZ_OK) {
            if (unwind_to_handler(vm, &status, depth, at, &chunk, &ip)) {
                continue;
            }
            return status;
        }
    }
    return LZ_OK;
}

LzStatus lz_vm_eval_with(LzVM *vm, const LzChunk *chunk, LzValue *result, LzExecuteFn run,
                         void *context) {
    size_t failed_at = 0;
    vm->error[0] = '\0';
    vm->error_offset = LZ_NO_OFFSET;
    vm->native_depth = 0;
    vm->trace_fixed = false;
    vm->heap.limit_hit = false;

    /* Frame 0 is the script itself: no closure, locals from the bottom of the stack. */
    vm->frame_count = 1;
    vm->frames[0] = (LzCallFrame){NULL, chunk, 0, 0, LZ_ALL_SUPPLIED, LZ_NO_OFFSET};

    LzStatus status = run(vm, chunk, &failed_at, context);
    if (status != LZ_OK) {
        /* The failing instruction belongs to the frame that was on top when it failed. */
        const LzChunk *failing = vm->frame_count > 0 ? vm->frames[vm->frame_count - 1].chunk : chunk;
        /* A failure inside a callback has already recorded its own offset. */
        if (failed_at < failing->count && vm->error_offset == LZ_NO_OFFSET) {
            vm->error_offset = failing->offsets[failed_at];
        }
        if (!vm->trace_fixed) {
            lz_vm_capture_trace(vm);
        }
    }
    if (status == LZ_OK && result != NULL) {
        if (vm->stack_count > 0) {
            *result = vm->stack[vm->stack_count - 1];
            lz_value_retain(result);
        } else {
            *result = lz_value_nil();
        }
    }
    lz_vm_reset_stack(vm);
    return status;
}

LzStatus lz_vm_eval(LzVM *vm, const LzChunk *chunk, LzValue *result) {
    return lz_vm_eval_with(vm, chunk, result, execute, NULL);
}

LzStatus lz_vm_run(LzVM *vm, const LzChunk *chunk) {
    return lz_vm_eval(vm, chunk, NULL);
}
