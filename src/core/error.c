#include "lizard/error.h"

#include "lizard/source.h"
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
    return lz_writer_write_cstr(writer, lz_error_kind_name((LzErrorKind)error->kind)) &&
           lz_writer_write_cstr(writer, ": ") &&
           lz_writer_write(writer, error->message->data, error->message->length);
}

LzStatus lz_error_new(LzVM *vm, LzErrorKind kind, const char *message, size_t length,
                      LzError **out) {
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
    error->message = text; /* the string's creation reference moves into the error */
    error->trace = NULL;
    error->cause = lz_value_nil();
    error->data = lz_value_nil();
    *out = error;
    return LZ_OK;
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
