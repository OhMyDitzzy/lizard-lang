#include "error/error.h"

#include "lizard/error.h"

/* The one argument is the message, and it must be a string. */
static LzStatus make_error(LzVM *vm, LzErrorKind kind, const LzValue *argv, LzValue *result) {
    const LzValue *message = &argv[0];
    if (message->type != LZ_VAL_OBJECT || message->as.object->type != LZ_OBJ_STRING) {
        return lz_vm_set_error(vm, LZ_ERR_TYPE, "the message of an error must be a string");
    }
    const LzString *text = (const LzString *)message->as.object;
    LzError *error = NULL;
    LzStatus status = lz_error_new(vm, kind, text->data, text->length, &error);
    if (status != LZ_OK) {
        return status;
    }
    *result = lz_value_object(&error->base); /* the creation reference moves to the caller */
    return LZ_OK;
}

#define DEFINE_CONSTRUCTOR(name, kind)                                                             \
    LzStatus lz_builtin_##name(LzVM *vm, int argc, const LzValue *argv, LzValue *result) {         \
        (void)argc;                                                                                \
        return make_error(vm, kind, argv, result);                                                 \
    }

DEFINE_CONSTRUCTOR(Error, LZ_ERROR_ERROR)
DEFINE_CONSTRUCTOR(RuntimeError, LZ_ERROR_RUNTIME)
DEFINE_CONSTRUCTOR(TypeError, LZ_ERROR_TYPE)
DEFINE_CONSTRUCTOR(ValueError, LZ_ERROR_VALUE)
DEFINE_CONSTRUCTOR(RangeError, LZ_ERROR_RANGE)
DEFINE_CONSTRUCTOR(ArithmeticError, LZ_ERROR_ARITHMETIC)
DEFINE_CONSTRUCTOR(ArityError, LZ_ERROR_ARITY)
DEFINE_CONSTRUCTOR(IOError, LZ_ERROR_IO)
