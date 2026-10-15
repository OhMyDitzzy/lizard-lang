#ifndef LIZARD_TEST_NATIVE_HELPERS_H
#define LIZARD_TEST_NATIVE_HELPERS_H

#include <string.h>

#include "lizard/native.h"
#include "lizard/vm.h"

static const LzNative *find_native(LzVM *vm, const char *name) {
    LzString *key = lz_vm_intern(vm, name, strlen(name));
    if (key == NULL) {
        return NULL;
    }
    const LzValue *value = lz_vm_lookup(vm, key);
    if (value == NULL || value->type != LZ_VAL_OBJECT || value->as.object->type != LZ_OBJ_NATIVE) {
        return NULL;
    }
    return (const LzNative *)value->as.object;
}

#endif
