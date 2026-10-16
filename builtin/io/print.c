#include "io/io.h"

/* Writing a long string is real work: one fuel unit per 64 bytes. */
static LzStatus charge_for_string(LzVM *vm, const LzValue *value) {
    if (value->type == LZ_VAL_OBJECT && value->as.object->type == LZ_OBJ_STRING) {
        return lz_vm_charge_fuel(vm, ((const LzString *)value->as.object)->length / 64);
    }
    return LZ_OK;
}

static LzStatus write_arguments(LzVM *vm, int argc, const LzValue *argv) {
    for (int i = 0; i < argc; i++) {
        LzStatus charged = charge_for_string(vm, &argv[i]);
        if (charged != LZ_OK) {
            return charged;
        }
        if (i > 0 && !lz_writer_write(&vm->out, " ", 1)) {
            return lz_vm_set_error(vm, LZ_ERR_IO, "failed to write to output");
        }
        if (!lz_value_write(&argv[i], &vm->out)) {
            return lz_vm_set_error(vm, LZ_ERR_IO, "failed to write to output");
        }
    }
    return LZ_OK;
}

LzStatus lz_builtin_print(LzVM *vm, int argc, const LzValue *argv, LzValue *result) {
    LzStatus status = write_arguments(vm, argc, argv);
    if (status == LZ_OK) {
        *result = lz_value_nil();
    }
    return status;
}

LzStatus lz_builtin_println(LzVM *vm, int argc, const LzValue *argv, LzValue *result) {
    LzStatus status = write_arguments(vm, argc, argv);
    if (status != LZ_OK) {
        return status;
    }
    if (!lz_writer_write(&vm->out, "\n", 1)) {
        return lz_vm_set_error(vm, LZ_ERR_IO, "failed to write to output");
    }
    *result = lz_value_nil();
    return LZ_OK;
}
