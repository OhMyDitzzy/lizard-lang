#include "method/method.h"

#include <stdio.h>

#include "lizard/dict.h"

/* The receiver of a dict method: the dispatcher already checked the kind. */
static LzDict *receiver(const LzValue *argv) {
    return lz_value_as_dict(&argv[0]);
}

LzStatus lz_method_dict_len(LzVM *vm, int argc, const LzValue *argv, LzValue *result) {
    (void)vm;
    (void)argc;
    *result = lz_value_int((int64_t)receiver(argv)->count);
    return LZ_OK;
}

LzStatus lz_method_dict_keys(LzVM *vm, int argc, const LzValue *argv, LzValue *result) {
    (void)argc;
    return lz_dict_keys(vm, receiver(argv), result);
}

LzStatus lz_method_dict_values(LzVM *vm, int argc, const LzValue *argv, LzValue *result) {
    (void)argc;
    return lz_dict_values(vm, receiver(argv), result);
}

LzStatus lz_method_dict_items(LzVM *vm, int argc, const LzValue *argv, LzValue *result) {
    (void)argc;
    return lz_dict_items(vm, receiver(argv), result);
}

/* d.get(k) is nil when the key is absent; d.get(k, default) is the default then. */
LzStatus lz_method_dict_get(LzVM *vm, int argc, const LzValue *argv, LzValue *result) {
    LzStatus status = lz_dict_own_key(vm, &argv[1]);
    if (status != LZ_OK) {
        return status;
    }
    const LzValue *stored = lz_dict_get(receiver(argv), &argv[1]);
    if (stored != NULL) {
        *result = *stored;
        lz_value_retain(result);
    } else if (argc > 2) {
        *result = argv[2];
        lz_value_retain(result);
    } else {
        *result = lz_value_nil();
    }
    return LZ_OK;
}

LzStatus lz_method_dict_has(LzVM *vm, int argc, const LzValue *argv, LzValue *result) {
    (void)argc;
    LzStatus status = lz_dict_own_key(vm, &argv[1]);
    if (status != LZ_OK) {
        return status;
    }
    *result = lz_value_bool(lz_dict_index_of(receiver(argv), &argv[1]) != SIZE_MAX);
    return LZ_OK;
}

LzStatus lz_method_dict_remove(LzVM *vm, int argc, const LzValue *argv, LzValue *result) {
    (void)argc;
    LzStatus status = lz_dict_own_key(vm, &argv[1]);
    if (status != LZ_OK) {
        return status;
    }
    status = lz_dict_remove(vm, receiver(argv), &argv[1], result);
    if (status != LZ_OK) {
        /* The message is "key ... not found": name the method it came from. */
        char message[sizeof vm->error];
        (void)snprintf(message, sizeof message, "%s", vm->error);
        LzSuggestions suggestions = vm->error_suggestions;
        status = lz_vm_set_error(vm, LZ_ERR_RUNTIME, "remove(): %s", message);
        vm->error_suggestions = suggestions;
    }
    return status;
}

LzStatus lz_method_dict_clear(LzVM *vm, int argc, const LzValue *argv, LzValue *result) {
    (void)argc;
    LzDict *dict = receiver(argv);
    lz_dict_clear(&vm->heap, dict);
    *result = lz_value_nil();
    return LZ_OK;
}

LzStatus lz_method_dict_copy(LzVM *vm, int argc, const LzValue *argv, LzValue *result) {
    (void)argc;
    return lz_dict_copy(vm, receiver(argv), result);
}

LzStatus lz_method_dict_merge(LzVM *vm, int argc, const LzValue *argv, LzValue *result) {
    (void)argc;
    if (!lz_value_is_dict(&argv[1])) {
        return lz_vm_set_error(vm, LZ_ERR_RUNTIME, "merge(): expected a dict, got %s",
                               lz_kind_name(lz_kind_of(&argv[1])));
    }
    return lz_dict_merge(vm, receiver(argv), lz_value_as_dict(&argv[1]), result);
}
