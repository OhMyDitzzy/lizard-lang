#include "lizard/error.h"
#include "lizard/class.h"

#include "lizard/dict.h"
#include "lizard/native.h"
#include "lizard/source.h"
#include "lizard/suggest.h"
#include "lizard/vm.h"

#include <stdio.h>

#include <string.h>

typedef struct {
    const char *name;
    LzErrorKind parent;
    bool has_parent;
} KindInfo;

static const KindInfo kinds[LZ_ERROR_KIND_COUNT] = {
    [LZ_ERROR_ERROR] = {"Error", LZ_ERROR_ERROR, false},
    [LZ_ERROR_RUNTIME] = {"RuntimeError", LZ_ERROR_ERROR, true},
    [LZ_ERROR_TYPE] = {"TypeError", LZ_ERROR_RUNTIME, true},
    [LZ_ERROR_VALUE] = {"ValueError", LZ_ERROR_RUNTIME, true},
    [LZ_ERROR_RANGE] = {"RangeError", LZ_ERROR_RUNTIME, true},
    [LZ_ERROR_ARITHMETIC] = {"ArithmeticError", LZ_ERROR_RUNTIME, true},
    [LZ_ERROR_ARITY] = {"ArityError", LZ_ERROR_RUNTIME, true},
    [LZ_ERROR_IO] = {"IOError", LZ_ERROR_ERROR, true},
};

const char *lz_error_kind_name(LzErrorKind kind) {
    return (unsigned)kind < LZ_ERROR_KIND_COUNT ? kinds[kind].name : "Error";
}

bool lz_error_kind_from_name(const char *name, size_t length, LzErrorKind *out) {
    for (unsigned i = 0; i < LZ_ERROR_KIND_COUNT; i++) {
        if (strlen(kinds[i].name) == length && memcmp(kinds[i].name, name, length) == 0) {
            *out = (LzErrorKind)i;
            return true;
        }
    }
    return false;
}

bool lz_error_kind_parent(LzErrorKind kind, LzErrorKind *parent) {
    if ((unsigned)kind >= LZ_ERROR_KIND_COUNT || !kinds[kind].has_parent) {
        return false;
    }
    *parent = kinds[kind].parent;
    return true;
}

bool lz_error_kind_is(LzErrorKind kind, LzErrorKind ancestor) {
    for (;;) {
        if (kind == ancestor) {
            return true;
        }
        if (!lz_error_kind_parent(kind, &kind)) {
            return false;
        }
    }
}

bool lz_status_catchable(LzStatus status) {
    switch (status) {
    case LZ_ERR_ARITY:
    case LZ_ERR_IO:
    case LZ_ERR_RUNTIME:
    case LZ_ERR_TYPE:
        return true;
    case LZ_OK:
    case LZ_ERR_MEMORY:
    case LZ_ERR_COMPILE:
    case LZ_ERR_HEAP_LIMIT:
    case LZ_ERR_FUEL:
    case LZ_ERR_STACK_OVERFLOW:
        return false;
    }
    return false;
}

static bool has(const char *message, const char *needle) {
    return strstr(message, needle) != NULL;
}

LzErrorKind lz_error_classify(LzStatus status, const char *message) {
    switch (status) {
    case LZ_ERR_ARITY:
        return LZ_ERROR_ARITY;
    case LZ_ERR_IO:
        return LZ_ERROR_IO;
    case LZ_ERR_TYPE:
        return LZ_ERROR_TYPE;
    case LZ_ERR_RUNTIME:
        break;
    default:
        return LZ_ERROR_RUNTIME;
    }

    /* Dividing by zero and leaving the range of an int are arithmetic. */
    if (has(message, "division by zero") || has(message, "overflow") ||
        has(message, "out of range for int") || has(message, "integer out of range") ||
        has(message, "float out of range") || has(message, "the result is not a number")) {
        return LZ_ERROR_ARITHMETIC;
    }
    /* A position or a key that is not there. */
    if (has(message, "out of range for array") || has(message, "index out of range") ||
        has(message, "key not found") || has(message, "not found") || has(message, "shift count must be") ||
        has(message, "bit index must be")) {
        return LZ_ERROR_RANGE;
    }
    /* A value of the wrong type. */
    if (has(message, "must be a number") || has(message, "must be numbers") ||
        has(message, "operands must be") || has(message, "needs an int") ||
        has(message, "needs two ints") || has(message, "cannot index a value") ||
        has(message, "cannot iterate over") || has(message, "cannot spread") ||
        has(message, "can only call functions") || has(message, "cannot use 'in' with") ||
        has(message, "the left side of 'in'") || has(message, "bounds must be ints") ||
        has(message, "index must be an int") || has(message, "cannot be a dict key") ||
        has(message, "cannot sort mixed types") || has(message, "cannot sort values of type") ||
        has(message, "has no method") || has(message, "has no field") || has(message, "tested with a range must be") ||
        has(message, "expected a ") || has(message, "expected an ") ||
        has(message, "type error:") || has(message, "must be a string") ||
        has(message, "only an error can be thrown")) {
        return LZ_ERROR_TYPE;
    }
    /* A value that is not acceptable although its type is. */
    if (has(message, "cannot convert") || has(message, "invalid integer") ||
        has(message, "invalid float") || has(message, "invalid spec") ||
        has(message, "format(): ") || has(message, "cannot be negative") ||
        has(message, "cannot be empty") || has(message, "cannot be 0") ||
        has(message, "square root of a negative") || has(message, "cannot compare nan") ||
        has(message, "cannot sort nan") || has(message, "array is empty") ||
        has(message, "too long") || has(message, "too large") || has(message, "too many") ||
        has(message, "inside itself") || has(message, "changed size during iteration") ||
        has(message, "remove(): ")) {
        return LZ_ERROR_VALUE;
    }
    return LZ_ERROR_RUNTIME;
}

bool lz_error_write(const LzError *error, const LzWriter *writer) {
    return lz_writer_write(writer, error->kind_name->data, error->kind_name->length) &&
           lz_writer_write_cstr(writer, ": ") &&
           lz_writer_write(writer, error->message->data, error->message->length);
}

/* The kinds of one VM */

size_t lz_vm_error_kind_count(const LzVM *vm) {
    return LZ_ERROR_KIND_COUNT + vm->error_type_count;
}

const char *lz_vm_error_kind_name(const LzVM *vm, unsigned kind) {
    if (kind < LZ_ERROR_KIND_COUNT) {
        return kinds[kind].name;
    }
    unsigned index = kind - LZ_ERROR_KIND_COUNT;
    return index < vm->error_type_count ? vm->error_types[index].name->data : "Error";
}

bool lz_vm_error_kind_from_name(const LzVM *vm, const char *name, size_t length, uint8_t *out) {
    LzErrorKind builtin;
    if (lz_error_kind_from_name(name, length, &builtin)) {
        *out = (uint8_t)builtin;
        return true;
    }
    for (size_t i = 0; i < vm->error_type_count; i++) {
        const LzString *declared = vm->error_types[i].name;
        if (declared->length == length && memcmp(declared->data, name, length) == 0) {
            *out = (uint8_t)(LZ_ERROR_KIND_COUNT + i);
            return true;
        }
    }
    return false;
}

bool lz_vm_error_kind_parent(const LzVM *vm, unsigned kind, uint8_t *parent) {
    if (kind < LZ_ERROR_KIND_COUNT) {
        LzErrorKind up;
        if (!lz_error_kind_parent((LzErrorKind)kind, &up)) {
            return false;
        }
        *parent = (uint8_t)up;
        return true;
    }
    unsigned index = kind - LZ_ERROR_KIND_COUNT;
    if (index >= vm->error_type_count) {
        return false;
    }
    *parent = vm->error_types[index].parent;
    return true;
}

bool lz_vm_error_kind_is(const LzVM *vm, unsigned kind, unsigned ancestor) {
    /* Every parent was declared before its child, so each step goes to a smaller number. */
    for (;;) {
        if (kind == ancestor) {
            return true;
        }
        uint8_t parent;
        if (!lz_vm_error_kind_parent(vm, kind, &parent)) {
            return false;
        }
        kind = parent;
    }
}

bool lz_native_error_kind(const LzNative *native, uint8_t *kind) {
    if (native->fn != lz_error_type_native) {
        return false;
    }
    if (kind != NULL) {
        *kind = (uint8_t)(uintptr_t)native->data;
    }
    return true;
}

LzStatus lz_vm_error_define_constructor(LzVM *vm, LzString *name, uint8_t kind) {
    LzNative *native = lz_native_new(&vm->heap, name, lz_error_type_native, 1, 1);
    if (native == NULL) {
        return lz_vm_out_of_memory(vm);
    }
    native->result_kinds = LZ_KIND_ERROR;
    native->data = (const void *)(uintptr_t)kind;
    bool defined = lz_vm_define_builtin(vm, name, lz_value_object(&native->base));
    lz_object_release(&vm->heap, &native->base);
    return defined ? LZ_OK : lz_vm_out_of_memory(vm);
}

LzStatus lz_vm_error_declare(LzVM *vm, LzString *name, uint8_t parent, uint8_t *out) {
    if (vm->error_type_count >= LZ_ERROR_TYPES_MAX - LZ_ERROR_KIND_COUNT) {
        return lz_vm_set_error(vm, LZ_ERR_RUNTIME, "too many error types (limit %d)",
                               LZ_ERROR_TYPES_MAX);
    }
    uint8_t kind = (uint8_t)(LZ_ERROR_KIND_COUNT + vm->error_type_count);
    LzStatus status = lz_vm_error_define_constructor(vm, name, kind);
    if (status != LZ_OK) {
        return status;
    }
    vm->error_types[vm->error_type_count].name = name;
    vm->error_types[vm->error_type_count].parent = parent;
    vm->error_types[vm->error_type_count].cls = NULL;
    vm->error_type_count++;
    *out = kind;
    return LZ_OK;
}

/* Values of an error */

LzStatus lz_error_new(LzVM *vm, unsigned kind, const char *message, size_t length,
                      LzError **out) {
    const char *kind_text = lz_vm_error_kind_name(vm, kind);
    LzString *kind_name = lz_vm_intern(vm, kind_text, strlen(kind_text));
    if (kind_name == NULL) {
        return lz_vm_out_of_memory(vm);
    }
    LzString *text = lz_string_new(&vm->heap, message, length);
    if (text == NULL) {
        return lz_vm_out_of_memory(vm);
    }
    LzError *error = lz_heap_alloc(&vm->heap, LZ_OBJ_ERROR, sizeof *error);
    if (error == NULL) {
        lz_object_release(&vm->heap, &text->base);
        return lz_vm_out_of_memory(vm);
    }
    error->kind = (uint8_t)kind;
    lz_object_retain(&kind_name->base);
    error->kind_name = kind_name;
    error->message = text; /* the string's creation reference moves into the error */
    error->trace = NULL;
    error->cause = lz_value_nil();
    error->data = lz_value_nil();
    error->payload = NULL;
    memset(error->is_kind, 0, sizeof error->is_kind);
    for (unsigned k = kind;; ) {
        error->is_kind[k / 64] |= (uint64_t)1 << (k % 64);
        uint8_t parent;
        if (!lz_vm_error_kind_parent(vm, k, &parent)) {
            break;
        }
        k = parent;
    }
    *out = error;
    return LZ_OK;
}

void lz_error_set_extra(LzVM *vm, LzError *error, const LzValue *cause, const LzValue *data) {
    lz_value_retain(cause);
    error->cause = *cause;
    lz_value_retain(data);
    error->data = *data;
    lz_note_container_store(&vm->heap, &error->data);
}

LzStatus lz_error_attach_trace(LzVM *vm, LzError *error) {
    if (error->trace != NULL) {
        return LZ_OK;
    }
    LzArray *trace = lz_array_new(&vm->heap, vm->trace_count);
    if (trace == NULL) {
        return lz_vm_out_of_memory(vm);
    }
    for (size_t i = 0; i < vm->trace_count && i < LZ_TRACE_MAX; i++) {
        const LzTraceEntry *entry = &vm->trace[i];
        char line[128];
        if (entry->offset != LZ_NO_OFFSET && vm->trace_source != NULL) {
            LzLocation at = lz_source_locate(vm->trace_source, entry->offset);
            (void)snprintf(line, sizeof line, "%s (%s:%u:%u)", entry->name, vm->trace_source->name,
                           (unsigned)at.line, (unsigned)at.column);
        } else {
            (void)snprintf(line, sizeof line, "%s", entry->name);
        }
        LzString *text = lz_string_new(&vm->heap, line, strlen(line));
        if (text == NULL) {
            lz_object_release(&vm->heap, &trace->base);
            return lz_vm_out_of_memory(vm);
        }
        LzValue value = lz_value_object(&text->base);
        bool added = lz_array_append(&vm->heap, trace, &value);
        lz_object_release(&vm->heap, &text->base); /* the array holds its own reference */
        if (!added) {
            lz_object_release(&vm->heap, &trace->base);
            return lz_vm_out_of_memory(vm);
        }
    }
    error->trace = trace;
    return LZ_OK;
}

/* Constructors */

static bool is_string(const LzValue *value) {
    return value->type == LZ_VAL_OBJECT && value->as.object->type == LZ_OBJ_STRING;
}

/* Checks the arguments and makes the error; the creation reference moves to *result. */
LzStatus lz_error_construct_core(LzVM *vm, uint8_t kind, const LzValue *message, const LzValue *cause,
                                 const LzValue *data, LzValue *result) {
    if (!is_string(message)) {
        return lz_vm_set_error(vm, LZ_ERR_TYPE, "the message of an error must be a string");
    }
    if (cause->type != LZ_VAL_NIL && !lz_value_is_error(cause)) {
        return lz_vm_set_error(vm, LZ_ERR_TYPE, "the cause of an error must be an error or nil");
    }
    const LzString *text = (const LzString *)message->as.object;
    LzError *error = NULL;
    LzStatus status = lz_error_new(vm, kind, text->data, text->length, &error);
    if (status != LZ_OK) {
        return status;
    }
    lz_error_set_extra(vm, error, cause, data);
    *result = lz_value_object(&error->base);
    return LZ_OK;
}

LzStatus lz_error_type_native(LzVM *vm, int argc, const LzValue *argv, LzValue *result) {
    /* current_native is valid at the entry only: read it before anything else runs. */
    uint8_t kind = (uint8_t)(uintptr_t)vm->current_native->data;
    LzClass *cls = lz_vm_error_class(vm, kind);
    if (cls != NULL) {
        return lz_error_class_construct(vm, cls, kind, (size_t)argc, argv, 0, NULL, 0, NULL, result);
    }
    LzValue nil = lz_value_nil();
    return lz_error_construct_core(vm, kind, &argv[0], &nil, &nil, result);
}

/* The index of the name [name, name + length) among the names the call can take, or -1. */
static int named_slot(const char *name, size_t length) {
    if (length == 5 && memcmp(name, "cause", 5) == 0) {
        return 0;
    }
    if (length == 4 && memcmp(name, "data", 4) == 0) {
        return 1;
    }
    return -1;
}

LzStatus lz_error_construct_named(LzVM *vm, uint8_t kind, size_t positional, const LzValue *argv,
                                  size_t named_count, const char *names, size_t names_length,
                                  const LzValue *named, LzValue *result) {
    const char *kind_name = lz_vm_error_kind_name(vm, kind);
    LzClass *cls = lz_vm_error_class(vm, kind);
    if (cls != NULL) {
        return lz_error_class_construct(vm, cls, kind, positional, argv, named_count, names, names_length,
                                        named, result);
    }
    if (positional != 1) {
        return lz_vm_set_error(vm, LZ_ERR_ARITY, "%s(): wrong number of arguments (%zu given)",
                               kind_name, positional);
    }
    LzValue given[2] = {lz_value_nil(), lz_value_nil()};
    bool seen[2] = {false, false};
    size_t position = 0;
    for (size_t k = 0; k < named_count; k++) {
        if (position > names_length) {
            return lz_vm_set_error(vm, LZ_ERR_RUNTIME, "malformed code: missing argument name");
        }
        size_t end = position;
        while (end < names_length && names[end] != '\0') {
            end++;
        }
        int slot = named_slot(names + position, end - position);
        if (slot < 0) {
            return lz_vm_set_error(vm, LZ_ERR_ARITY, "%s(): unexpected named argument '%.*s' (an error takes cause and data)",
                                   kind_name, (int)(end - position), names + position);
        }
        if (seen[slot]) {
            return lz_vm_set_error(vm, LZ_ERR_ARITY, "%s(): got multiple values for argument '%s'",
                                   kind_name, slot == 0 ? "cause" : "data");
        }
        seen[slot] = true;
        given[slot] = named[k];
        position = end + 1;
    }
    return lz_error_construct_core(vm, kind, &argv[0], &given[0], &given[1], result);
}

/* Classes of declared kinds */

LzClass *lz_vm_error_class(const LzVM *vm, unsigned kind) {
    if (kind < LZ_ERROR_KIND_COUNT || (size_t)(kind - LZ_ERROR_KIND_COUNT) >= vm->error_type_count) {
        return NULL;
    }
    return vm->error_types[kind - LZ_ERROR_KIND_COUNT].cls;
}

LzStatus lz_vm_error_set_class(LzVM *vm, uint8_t kind, LzClass *cls) {
    if (kind < LZ_ERROR_KIND_COUNT || (size_t)(kind - LZ_ERROR_KIND_COUNT) >= vm->error_type_count) {
        return lz_vm_set_error(vm, LZ_ERR_RUNTIME, "only a declared error kind can have a class");
    }
    LzErrorType *type = &vm->error_types[kind - LZ_ERROR_KIND_COUNT];
    if (type->cls == cls) {
        return LZ_OK; /* the statement ran before */
    }
    lz_object_retain(&cls->base);
    LzClass *old = type->cls;
    type->cls = cls;
    if (old != NULL) {
        lz_object_release(&vm->heap, &old->base);
    }
    lz_vm_error_widen_constructor(vm, kind);
    return LZ_OK;
}

void lz_vm_error_widen_constructor(LzVM *vm, uint8_t kind) {
    const LzValue *found = lz_vm_lookup(vm, vm->error_types[kind - LZ_ERROR_KIND_COUNT].name);
    if (found != NULL && found->type == LZ_VAL_OBJECT && found->as.object->type == LZ_OBJ_NATIVE) {
        ((LzNative *)found->as.object)->max_args = 1 + 64; /* the fields are checked when it runs */
    }
}

void lz_vm_release_error_classes(LzVM *vm) {
    for (size_t i = 0; i < vm->error_type_count; i++) {
        LzClass *cls = vm->error_types[i].cls;
        vm->error_types[i].cls = NULL;
        if (cls != NULL) {
            lz_object_release(&vm->heap, &cls->base);
        }
    }
}
