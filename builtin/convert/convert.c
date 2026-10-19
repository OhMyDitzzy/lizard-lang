#include "convert/convert.h"

#include <math.h>

#include "lizard/number.h"

#define SHOWN_BYTES 32

static bool is_string(const LzValue *value) {
    return value->type == LZ_VAL_OBJECT && value->as.object->type == LZ_OBJ_STRING;
}

static const char *kind_of(const LzValue *value) {
    return lz_kind_name(lz_kind_of(value));
}

static int shown(const LzString *text) {
    return (int)(text->length < SHOWN_BYTES ? text->length : SHOWN_BYTES);
}

static const char *ellipsis(const LzString *text) {
    return text->length > SHOWN_BYTES ? "..." : "";
}

LzStatus lz_builtin_int(LzVM *vm, int argc, const LzValue *argv, LzValue *result) {
    (void)argc;
    const LzValue *value = &argv[0];

    if (value->type == LZ_VAL_INT) {
        *result = *value;
        return LZ_OK;
    }
    if (value->type == LZ_VAL_FLOAT) {
        double number = value->as.number;
        if (isnan(number) || isinf(number)) {
            return lz_vm_set_error(vm, LZ_ERR_RUNTIME, "int(): cannot convert %s",
                                   isnan(number) ? "nan" : "infinity");
        }
        if (number >= 9223372036854775808.0 || number < -9223372036854775808.0) {
            return lz_vm_set_error(vm, LZ_ERR_RUNTIME, "int(): float out of range for int");
        }
        *result = lz_value_int((int64_t)number);
        return LZ_OK;
    }
    if (is_string(value)) {
        const LzString *text = (const LzString *)value->as.object;
        int64_t parsed = 0;
        LzParseResult status = lz_parse_integer(text->data, text->length, &parsed);
        if (status == LZ_PARSE_RANGE) {
            return lz_vm_set_error(vm, LZ_ERR_RUNTIME, "int(): integer out of range");
        }
        if (status != LZ_PARSE_OK) {
            return lz_vm_set_error(vm, LZ_ERR_RUNTIME, "int(): invalid integer \"%.*s%s\"",
                                   shown(text), text->data, ellipsis(text));
        }
        *result = lz_value_int(parsed);
        return LZ_OK;
    }
    return lz_vm_set_error(vm, LZ_ERR_RUNTIME, "int(): cannot convert %s", kind_of(value));
}

LzStatus lz_builtin_float(LzVM *vm, int argc, const LzValue *argv, LzValue *result) {
    (void)argc;
    const LzValue *value = &argv[0];

    if (value->type == LZ_VAL_FLOAT) {
        *result = *value;
        return LZ_OK;
    }
    if (value->type == LZ_VAL_INT) {
        *result = lz_value_float((double)value->as.integer);
        return LZ_OK;
    }
    if (is_string(value)) {
        const LzString *text = (const LzString *)value->as.object;
        double parsed = 0;
        LzParseResult status = lz_parse_float(text->data, text->length, &parsed);
        if (status == LZ_PARSE_NO_MEMORY) {
            return lz_vm_out_of_memory(vm);
        }
        if (status == LZ_PARSE_RANGE) {
            return lz_vm_set_error(vm, LZ_ERR_RUNTIME, "float(): float out of range");
        }
        if (status != LZ_PARSE_OK) {
            return lz_vm_set_error(vm, LZ_ERR_RUNTIME, "float(): invalid float \"%.*s%s\"",
                                   shown(text), text->data, ellipsis(text));
        }
        *result = lz_value_float(parsed);
        return LZ_OK;
    }
    return lz_vm_set_error(vm, LZ_ERR_RUNTIME, "float(): cannot convert %s", kind_of(value));
}

LzStatus lz_builtin_string(LzVM *vm, int argc, const LzValue *argv, LzValue *result) {
    (void)argc;
    const LzValue *value = &argv[0];

    if (is_string(value)) {
        lz_value_retain(value);
        *result = *value;
        return LZ_OK;
    }

    LzBuffer text = {0};
    LzWriter writer = lz_writer_from_buffer(&text);
    if (!lz_value_write(value, &writer)) {
        lz_buffer_free(&text);
        return lz_vm_out_of_memory(vm);
    }

    LzString *string = lz_string_new(&vm->heap, text.data, text.length);
    lz_buffer_free(&text);
    if (string == NULL) {
        return lz_vm_out_of_memory(vm);
    }
    *result = lz_value_object(&string->base);
    return LZ_OK;
}

LzStatus lz_builtin_bool(LzVM *vm, int argc, const LzValue *argv, LzValue *result) {
    (void)vm;
    (void)argc;
    *result = lz_value_bool(lz_value_is_truthy(&argv[0]));
    return LZ_OK;
}
