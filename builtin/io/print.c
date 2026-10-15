#include "io/io.h"

static LzStatus write_arguments(LzVM *vm, int argc, const LzValue *argv) {
    for (int i = 0; i < argc; i++) {
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
