/*
 * `extend Target { ... }`: methods and properties added to a type that already exists.
 *
 * Each block is a hidden class (LzClass.is_ext) kept in vm->exts once its statement has run. The
 * block is also the "class" its code is written in, so the private members of a block are visible
 * only to that block, and the code in it sees only the public members of the target.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lizard/class.h"
#include "lizard/error.h"
#include "lizard/native.h"
#include "lizard/vm.h"

static const LzMember *ext_member(const LzClass *ext, const LzString *name) {
    for (size_t i = 0; i < ext->member_count; i++) {
        if (ext->members[i].name == name && ext->members[i].is_required == 0) {
            return &ext->members[i];
        }
    }
    return NULL;
}

bool lz_ext_find_kind(const LzVM *vm, uint16_t kind, const LzString *name, LzFound *out) {
    for (size_t i = 0; i < vm->ext_count; i++) {
        const LzClass *ext = vm->exts[i];
        if ((ext->ext_kinds & kind) == 0) {
            continue;
        }
        const LzMember *member = ext_member(ext, name);
        if (member != NULL) {
            out->member = member;
            out->declared_in = ext;
            return true;
        }
    }
    return false;
}

/* For a value: the extensions of its kind; for an error also those of its kind of error and its ancestors. */
bool lz_ext_find_value(const LzVM *vm, const LzValue *value, const LzString *name, LzFound *out) {
    uint16_t kind = lz_kind_of(value);
    if (kind == LZ_KIND_ERROR) {
        const LzError *error = (const LzError *)value->as.object;
        for (size_t i = 0; i < vm->ext_count; i++) {
            const LzClass *ext = vm->exts[i];
            if (ext->ext_error_kind == 0) {
                continue;
            }
            size_t bit = (size_t)(ext->ext_error_kind - 1);
            if (((error->is_kind[bit / 64] >> (bit % 64)) & 1u) == 0) {
                continue;
            }
            const LzMember *member = ext_member(ext, name);
            if (member != NULL) {
                out->member = member;
                out->declared_in = ext;
                return true;
            }
        }
        return false;
    }
    return lz_ext_find_kind(vm, kind, name, out);
}

bool lz_ext_find_class(const LzVM *vm, const LzClass *cls, const LzString *name, LzFound *out) {
    if (vm->ext_count == 0) {
        return false;
    }
    for (const LzClass *c = cls; c != NULL; c = c->parent) {
        for (size_t i = 0; i < vm->ext_count; i++) {
            const LzClass *ext = vm->exts[i];
            if (ext->ext_target_id != c->id) {
                continue;
            }
            const LzMember *member = ext_member(ext, name);
            if (member != NULL) {
                out->member = member;
                out->declared_in = ext;
                return true;
            }
        }
    }
    return false;
}

static LzStatus ext_fail(LzVM *vm, const char *help, const char *format, ...) LZ_PRINTF_FORMAT(3, 4);

static LzStatus ext_fail(LzVM *vm, const char *help, const char *format, ...) {
    char message[256];
    va_list args;
    va_start(args, format);
    (void)vsnprintf(message, sizeof message, format, args);
    va_end(args);
    (void)lz_vm_set_error(vm, LZ_ERR_RUNTIME, "%s", message);
    vm->error_kind = (uint8_t)LZ_ERROR_RUNTIME;
    if (help != NULL) {
        (void)snprintf(vm->error_help, sizeof vm->error_help, "%s", help);
    }
    return LZ_ERR_RUNTIME;
}

static const unsigned single_kinds[] = {LZ_KIND_NIL,    LZ_KIND_BOOL,  LZ_KIND_INT,   LZ_KIND_FLOAT,
                                        LZ_KIND_STRING, LZ_KIND_FUNCTION, LZ_KIND_ARRAY, LZ_KIND_DICT};

LzStatus lz_vm_extend_register(LzVM *vm, LzClass *ext, const LzValue *target) {
    const char *target_name = ext->ext_target->data;
    const LzClass *target_class = NULL;
    uint8_t error_kind = 0;
    if (target != NULL && target->type == LZ_VAL_OBJECT && target->as.object->type == LZ_OBJ_NATIVE &&
        lz_native_error_kind((const LzNative *)target->as.object, &error_kind)) {
        /* An error kind (Error, ValueError, one of the script's): its errors and those of its descendants. */
        ext->ext_error_kind = (uint16_t)error_kind + 1;
        for (size_t m = 0; m < ext->member_count; m++) {
            const LzString *name = ext->members[m].name;
            const char *text = name->data;
            if (strcmp(text, "message") == 0 || strcmp(text, "kind") == 0 || strcmp(text, "cause") == 0 ||
                strcmp(text, "data") == 0 || strcmp(text, "trace") == 0 ||
                (vm->method_row_check != NULL && vm->method_row_check(text, LZ_KIND_ERROR))) {
                return ext_fail(vm, "it is a builtin member", "'%s' is already defined for %s", text, target_name);
            }
            for (size_t e = 0; e < vm->ext_count; e++) {
                const LzClass *other = vm->exts[e];
                if (other->ext_error_kind == ext->ext_error_kind && ext_member(other, name) != NULL) {
                    return ext_fail(vm, "an earlier extend already added it", "'%s' is already defined for %s", text,
                                    target_name);
                }
            }
        }
        target_class = NULL;
    } else if (target != NULL) {
        if (!lz_value_is_class(target)) {
            return ext_fail(vm, NULL, "'%s' is not a type that can be extended", target_name);
        }
        target_class = lz_value_as_class(target);
        if (target_class->is_trait) {
            return ext_fail(vm, NULL, "extending a trait is not supported yet");
        }
        ext->ext_target_id = target_class->id;
    }
    for (size_t m = 0; ext->ext_error_kind == 0 && m < ext->member_count; m++) {
        const LzMember *member = &ext->members[m];
        const LzString *name = member->name;
        char help[160];
        if (target_class != NULL) {
            LzFound hit;
            if (lz_class_lookup(target_class, name, &hit)) {
                (void)snprintf(help, sizeof help, "%s is already a member of %s",
                               name->data, hit.declared_in->name->data);
                return ext_fail(vm, hit.declared_in->is_ext ? "an earlier extend already added it" : help,
                                "'%s' is already defined for %s", name->data, target_name);
            }
            if (strcmp(name->data, "with") == 0 || strcmp(name->data, "copy") == 0 ||
                (vm->method_row_check != NULL && vm->method_row_check(name->data, LZ_KIND_INSTANCE))) {
                return ext_fail(vm, "it is a builtin method", "'%s' is already defined for %s", name->data,
                                target_name);
            }
        } else {
            for (size_t k = 0; k < sizeof single_kinds / sizeof *single_kinds; k++) {
                unsigned kind = single_kinds[k];
                if ((ext->ext_kinds & kind) == 0) {
                    continue;
                }
                LzFound hit;
                if (lz_ext_find_kind(vm, (uint16_t)kind, name, &hit)) {
                    return ext_fail(vm, "an earlier extend already added it", "'%s' is already defined for %s",
                                    name->data, target_name);
                }
                if (vm->method_row_check != NULL && vm->method_row_check(name->data, kind)) {
                    return ext_fail(vm, "it is a builtin method", "'%s' is already defined for %s", name->data,
                                    target_name);
                }
            }
        }
    }
    if (vm->ext_count == vm->ext_capacity) {
        size_t capacity = vm->ext_capacity == 0 ? 8 : vm->ext_capacity * 2;
        LzClass **grown = realloc(vm->exts, capacity * sizeof *grown);
        if (grown == NULL) {
            return lz_vm_out_of_memory(vm);
        }
        vm->exts = grown;
        vm->ext_capacity = capacity;
    }
    lz_object_retain(&ext->base);
    vm->exts[vm->ext_count++] = ext;
    vm->ext_version++;
    return LZ_OK;
}

void lz_vm_release_extensions(LzVM *vm) {
    for (size_t i = 0; i < vm->ext_count; i++) {
        lz_object_release(&vm->heap, &vm->exts[i]->base);
    }
    free(vm->exts);
    vm->exts = NULL;
    vm->ext_count = 0;
    vm->ext_capacity = 0;
}
