#include "lizard/class.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lizard/array.h"
#include "lizard/dict.h"
#include "lizard/native.h"
#include "lizard/suggest.h"

/* ---- building a class ---- */

LzClass *lz_class_new(LzVM *vm, LzString *name, uint32_t offset) {
    LzClass *cls = lz_heap_alloc(&vm->heap, LZ_OBJ_CLASS, sizeof *cls);
    if (cls == NULL) {
        return NULL;
    }
    cls->name = name;
    lz_object_retain(&name->base);
    cls->parent = NULL;
    cls->vm = vm;
    cls->members = NULL;
    cls->member_count = 0;
    cls->member_capacity = 0;
    cls->field_count = 0;
    cls->param_count = 0;
    cls->init_member = -1;
    cls->consts_ready = false;
    cls->offset = offset;
    return cls;
}

bool lz_class_add_member(LzClass *cls, LzString *name, LzMemberKind kind, LzVisibility vis,
                         uint32_t offset, LzMember **out) {
    if (cls->member_count == cls->member_capacity) {
        size_t capacity = cls->member_capacity == 0 ? 8 : cls->member_capacity * 2;
        LzMember *grown = realloc(cls->members, capacity * sizeof *grown);
        if (grown == NULL) {
            return false;
        }
        cls->members = grown;
        cls->member_capacity = capacity;
    }
    LzMember *member = &cls->members[cls->member_count++];
    memset(member, 0, sizeof *member);
    member->name = name;
    lz_object_retain(&name->base);
    member->kind = (uint8_t)kind;
    member->vis = (uint8_t)vis;
    member->value = lz_value_nil();
    member->thunk = lz_value_nil();
    member->type = lz_type_of_kinds(LZ_KINDS_ANY);
    member->offset = offset;
    if (kind == LZ_MEMBER_FIELD) {
        member->slot = (uint16_t)cls->field_count++;
    }
    if (out != NULL) {
        *out = member;
    }
    return true;
}

void lz_class_seal(LzClass *cls) {
    cls->param_count = 0;
    cls->init_member = -1;
    for (size_t i = 0; i < cls->member_count; i++) {
        const LzMember *member = &cls->members[i];
        if (member->kind == LZ_MEMBER_FIELD && member->vis != LZ_VIS_PRIVATE) {
            cls->params[cls->param_count++] = (uint16_t)i;
        }
        if (member->kind == LZ_MEMBER_METHOD && strcmp(member->name->data, "init") == 0) {
            cls->init_member = (int)i;
        }
    }
}

const LzMember *lz_class_find(const LzClass *cls, const LzString *name) {
    for (size_t i = 0; i < cls->member_count; i++) {
        if (cls->members[i].name == name) {
            return &cls->members[i];
        }
    }
    return NULL;
}

bool lz_class_sees(const LzString *owner, const LzClass *cls) {
    /* Stage 2: also true when `owner` names a class derived from cls. */
    return owner != NULL && owner == cls->name;
}

const char *lz_class_display_name(const LzClass *cls) {
    return cls->name->data;
}

void lz_instance_drop_holder(const LzValue *item) {
    lz_value_as_instance(item)->holders--;
}

/* ---- finalizers ---- */

void lz_class_finalize(LzHeap *heap, LzClass *cls) {
    for (size_t i = 0; i < cls->member_count; i++) {
        LzMember *member = &cls->members[i];
        lz_value_release(heap, &member->value);
        lz_value_release(heap, &member->thunk);
        lz_object_release(heap, &member->name->base);
    }
    free(cls->members);
    cls->members = NULL;
    lz_object_release(heap, &cls->name->base);
}

void lz_instance_finalize(LzHeap *heap, LzInstance *instance) {
    size_t count = instance->cls->field_count;
    for (size_t i = 0; i < count; i++) {
        lz_drop_container_holder(&instance->fields[i]);
        lz_value_release(heap, &instance->fields[i]);
    }
    lz_object_release(heap, &instance->cls->base);
}

void lz_bound_finalize(LzHeap *heap, LzBound *bound) {
    lz_value_release(heap, &bound->callee);
    lz_value_release(heap, &bound->receiver);
}

LzBound *lz_bound_new(LzHeap *heap, const LzValue *callee, const LzValue *receiver, bool skip_first) {
    LzBound *bound = lz_heap_alloc(heap, LZ_OBJ_BOUND, sizeof *bound);
    if (bound == NULL) {
        return NULL;
    }
    bound->callee = *callee;
    lz_value_retain(&bound->callee);
    bound->receiver = receiver != NULL ? *receiver : lz_value_nil();
    lz_value_retain(&bound->receiver);
    bound->skip_first = skip_first;
    return bound;
}

void lz_vm_release_class_constants(LzVM *vm) {
    size_t count = 0;
    for (LzObject *o = vm->heap.head; o != NULL; o = o->next) {
        if (o->type == LZ_OBJ_CLASS) {
            count++;
        }
    }
    if (count == 0) {
        return;
    }
    LzClass **classes = malloc(count * sizeof *classes);
    if (classes == NULL) {
        return;
    }
    size_t n = 0;
    for (LzObject *o = vm->heap.head; o != NULL && n < count; o = o->next) {
        if (o->type == LZ_OBJ_CLASS) {
            lz_object_retain(o);
            classes[n++] = (LzClass *)o;
        }
    }
    for (size_t i = 0; i < n; i++) {
        for (size_t k = 0; k < classes[i]->member_count; k++) {
            LzMember *member = &classes[i]->members[k];
            if (member->kind == LZ_MEMBER_CONST) {
                LzValue old = member->value;
                member->value = lz_value_nil();
                lz_value_release(&vm->heap, &old);
            }
        }
    }
    for (size_t i = 0; i < n; i++) {
        lz_object_release(&vm->heap, &classes[i]->base);
    }
    free(classes);
}

/* ---- errors ---- */

static LzStatus fail(LzVM *vm, LzStatus status, const char *help, const char *format, ...)
    LZ_PRINTF_FORMAT(4, 5);

static LzStatus fail(LzVM *vm, LzStatus status, const char *help, const char *format, ...) {
    char message[256];
    va_list args;
    va_start(args, format);
    (void)vsnprintf(message, sizeof message, format, args);
    va_end(args);
    (void)lz_vm_set_error(vm, status, "%s", message);
    if (status == LZ_ERR_RUNTIME) {
        vm->error_kind = (uint8_t)LZ_ERROR_RUNTIME; /* the wording must not pick another kind */
    }
    if (help != NULL) {
        (void)snprintf(vm->error_help, sizeof vm->error_help, "%s", help);
    }
    return status;
}

static bool member_visible(const LzString *owner, const LzClass *cls, const LzMember *member) {
    return member->vis != LZ_VIS_PRIVATE || lz_class_sees(owner, cls);
}

/* " (did you mean 'x'?)" for a name that is not a member, from the members the caller can see. */
static void did_you_mean(const LzClass *cls, const LzString *owner, const LzString *name,
                         bool fields_only, char *out, size_t size) {
    out[0] = '\0';
    LzSuggester suggester;
    lz_suggester_init(&suggester, name->data, name->length);
    for (size_t i = 0; i < cls->member_count; i++) {
        const LzMember *member = &cls->members[i];
        if (fields_only && member->kind != LZ_MEMBER_FIELD) {
            continue;
        }
        if (member_visible(owner, cls, member)) {
            lz_suggester_offer(&suggester, member->name->data);
        }
    }
    size_t count = suggester.found.count;
    if (count == 0) {
        return;
    }
    size_t used = (size_t)snprintf(out, size, " (did you mean ");
    for (size_t i = 0; i < count && used < size; i++) {
        used += (size_t)snprintf(out + used, size - used, "%s'%s'", i == 0 ? "" : (i + 1 == count ? " or " : ", "),
                                 suggester.found.names[i]);
    }
    if (used < size) {
        (void)snprintf(out + used, size - used, "?)");
    }
}

static LzStatus no_member(LzVM *vm, const LzClass *cls, const LzString *owner, const LzString *name,
                          const char *what, bool fields_only, bool write) {
    char suggestion[160];
    did_you_mean(cls, owner, name, fields_only, suggestion, sizeof suggestion);
    return fail(vm, LZ_ERR_RUNTIME, write ? "fields are declared in the class" : NULL,
                "%s has no %s '%s'%s", cls->name->data, what, name->data, suggestion);
}

static LzStatus private_error(LzVM *vm, const LzClass *cls, const LzMember *member) {
    const char *help = member->kind == LZ_MEMBER_FIELD || member->kind == LZ_MEMBER_GETTER
                           ? "mark it 'pub' (or 'pub read') to use it from outside"
                           : (member->kind == LZ_MEMBER_CONST ? "mark it 'pub' to use it from outside"
                                                              : "mark it 'pub' to call it from outside");
    if (member->kind == LZ_MEMBER_GETTER) {
        help = "mark it 'pub' to use it from outside";
    }
    return fail(vm, LZ_ERR_RUNTIME, help, "'%s' is private to %s", member->name->data, cls->name->data);
}

static LzStatus read_only_error(LzVM *vm, const LzClass *cls, const LzMember *member) {
    char help[160];
    const char *method = NULL;
    for (size_t i = 0; i < cls->member_count; i++) {
        const LzMember *m = &cls->members[i];
        if (m->kind == LZ_MEMBER_METHOD && m->vis != LZ_VIS_PRIVATE && strcmp(m->name->data, "to_string") != 0 &&
            strcmp(m->name->data, "to_debug") != 0) {
            method = m->name->data;
            break;
        }
    }
    if (method != NULL) {
        (void)snprintf(help, sizeof help, "change it inside a method such as %s()", method);
    } else {
        (void)snprintf(help, sizeof help, "change it inside a method of %s", cls->name->data);
    }
    return fail(vm, LZ_ERR_RUNTIME, help, "'%s' is read-only outside %s", member->name->data,
                cls->name->data);
}

static LzStatus unset_error(LzVM *vm, const LzClass *cls, const LzMember *member) {
    return fail(vm, LZ_ERR_RUNTIME, NULL, "field '%s' of %s has no value yet (assign it in init())",
                member->name->data, cls->name->data);
}

/* ---- instances ---- */

static LzInstance *instance_alloc(LzVM *vm, LzClass *cls) {
    size_t size = sizeof(LzInstance) + cls->field_count * sizeof(LzValue);
    LzInstance *instance = lz_heap_alloc(&vm->heap, LZ_OBJ_INSTANCE, size);
    if (instance == NULL) {
        return NULL;
    }
    lz_object_retain(&cls->base);
    instance->cls = cls;
    instance->holders = 0;
    instance->mark = 0;
    instance->unset = cls->field_count >= 64 ? UINT64_MAX : (((uint64_t)1 << cls->field_count) - 1u);
    for (size_t i = 0; i < cls->field_count; i++) {
        instance->fields[i] = lz_value_nil();
    }
    return instance;
}

/* What the value is, for "expected X, got Y": a type for containers and literals, else its kind. */
static void describe_actual(const LzType *type, const LzValue *value, const LzWriter *out) {
    if (lz_value_is_array(value) || lz_value_is_dict(value)) {
        LzType shown = lz_type_of_value(value);
        lz_type_describe(&shown, out);
    } else if (type->literal_count > 0) {
        lz_value_describe(value, out);
    } else {
        (void)lz_writer_write_cstr(out, lz_value_kind_name(value));
    }
}

static LzStatus check_field_type(LzVM *vm, const LzClass *cls, const LzMember *member,
                                 const LzValue *value) {
    const LzType *type = &member->type;
    bool fits;
    if (lz_value_is_array(value)) {
        LzStatus charged = lz_vm_charge_fuel(vm, lz_array_cost(lz_value_as_array(value)->count));
        if (charged != LZ_OK) {
            return charged;
        }
        fits = (type->kinds & LZ_KIND_ARRAY) != 0 && lz_array_conform(lz_value_as_array(value), type->element);
    } else {
        fits = lz_type_matches(type, value);
    }
    if (fits) {
        return LZ_OK;
    }
    LzBuffer expected = {0};
    LzBuffer actual = {0};
    LzWriter expected_out = lz_writer_from_buffer(&expected);
    LzWriter actual_out = lz_writer_from_buffer(&actual);
    lz_type_describe(type, &expected_out);
    describe_actual(type, value, &actual_out);
    LzStatus status = fail(vm, LZ_ERR_TYPE, NULL, "%s.%s expects %s, got %s", cls->name->data,
                           member->name->data, expected.data != NULL ? expected.data : "?",
                           actual.data != NULL ? actual.data : "?");
    lz_buffer_free(&expected);
    lz_buffer_free(&actual);
    return status;
}

LzStatus lz_instance_store(LzVM *vm, LzInstance *instance, const LzMember *member,
                           const LzValue *value) {
    LzClass *cls = instance->cls;
    LzStatus status = check_field_type(vm, cls, member, value);
    if (status != LZ_OK) {
        return status;
    }
    if (lz_value_may_hold_container(value)) {
        uint64_t work = 0;
        int cycle = lz_containers_would_cycle(&vm->heap, value, &instance->base, &work);
        status = lz_vm_charge_fuel(vm, lz_array_cost((size_t)work));
        if (status != LZ_OK) {
            return status;
        }
        if (cycle < 0) {
            return lz_vm_out_of_memory(vm);
        }
        if (cycle > 0) {
            return fail(vm, LZ_ERR_RUNTIME, "a value cannot contain itself; keep an id or a name instead",
                        "cannot store: this would create a reference cycle (through field '%s' of %s)",
                        member->name->data, cls->name->data);
        }
    }
    LzValue stored = *value;
    lz_value_retain(&stored);
    LzValue old = instance->fields[member->slot];
    lz_drop_container_holder(&old);
    instance->fields[member->slot] = stored;
    lz_note_container_store(&vm->heap, &stored);
    instance->unset &= ~((uint64_t)1 << member->slot);
    lz_value_release(&vm->heap, &old);
    return LZ_OK;
}

/* ---- running script code from C ---- */

static LzStatus call_closure(LzVM *vm, const LzValue *callee, size_t argc, const LzValue *argv,
                             LzValue *result) {
    return lz_vm_call_value(vm, callee, argc, argv, result);
}

LzStatus lz_class_run_consts(LzVM *vm, LzClass *cls) {
    if (cls->consts_ready) {
        return LZ_OK;
    }
    cls->consts_ready = true;
    lz_object_retain(&cls->base);
    LzStatus status = LZ_OK;
    for (size_t i = 0; i < cls->member_count && status == LZ_OK; i++) {
        LzMember *member = &cls->members[i];
        if (member->kind != LZ_MEMBER_CONST || member->thunk.type == LZ_VAL_NIL) {
            continue;
        }
        LzValue value = lz_value_nil();
        status = call_closure(vm, &member->thunk, 0, NULL, &value);
        if (status == LZ_OK) {
            LzValue old = member->value;
            member->value = value;
            lz_value_release(&vm->heap, &old);
        }
    }
    lz_object_release(&vm->heap, &cls->base);
    return status;
}

static LzStatus finish_instance(LzVM *vm, LzInstance *instance) {
    LzClass *cls = instance->cls;
    if (cls->init_member >= 0) {
        LzValue self = lz_value_object(&instance->base);
        LzValue ignored = lz_value_nil();
        LzStatus status = call_closure(vm, &cls->members[cls->init_member].value, 1, &self, &ignored);
        if (status != LZ_OK) {
            return status;
        }
        lz_value_release(&vm->heap, &ignored);
    }
    if (instance->unset != 0) {
        for (size_t i = 0; i < cls->member_count; i++) {
            const LzMember *member = &cls->members[i];
            if (member->kind == LZ_MEMBER_FIELD && ((instance->unset >> member->slot) & 1u) != 0) {
                return unset_error(vm, cls, member);
            }
        }
    }
    return LZ_OK;
}

/* Which parameter of the constructor is `name`: a field, public. Fails with the right message. */
static LzStatus param_of(LzVM *vm, const LzClass *cls, const LzString *name, size_t *param,
                         const char *callname) {
    for (size_t i = 0; i < cls->param_count; i++) {
        if (cls->members[cls->params[i]].name == name) {
            *param = i;
            return LZ_OK;
        }
    }
    const LzMember *member = lz_class_find(cls, name);
    if (member != NULL && member->kind == LZ_MEMBER_FIELD) {
        return fail(vm, LZ_ERR_ARITY, NULL, "'%s' is private to %s, so it is not an argument of %s(...)",
                    name->data, cls->name->data, callname);
    }
    char suggestion[160];
    suggestion[0] = '\0';
    LzSuggester suggester;
    lz_suggester_init(&suggester, name->data, name->length);
    for (size_t i = 0; i < cls->param_count; i++) {
        lz_suggester_offer(&suggester, cls->members[cls->params[i]].name->data);
    }
    if (suggester.found.count > 0) {
        (void)snprintf(suggestion, sizeof suggestion, " (did you mean '%s'?)", suggester.found.names[0]);
    }
    return fail(vm, LZ_ERR_ARITY, NULL, "%s has no field '%s'%s", cls->name->data, name->data, suggestion);
}

static const LzString *name_at(LzVM *vm, const char *names, size_t length, size_t index,
                               LzString **out) {
    size_t position = 0;
    for (size_t k = 0; k < index; k++) {
        while (position < length && names[position] != '\0') {
            position++;
        }
        position++;
    }
    size_t end = position;
    while (end < length && names[end] != '\0') {
        end++;
    }
    *out = lz_vm_intern(vm, names + position, end - position);
    return *out;
}

LzStatus lz_class_construct(LzVM *vm, LzClass *cls, size_t positional, const LzValue *argv,
                            size_t named_count, const char *names, size_t names_length,
                            const LzValue *named, LzValue *result) {
    if (positional > cls->param_count) {
        return fail(vm, LZ_ERR_ARITY, NULL, "%s takes at most %zu argument%s, got %zu", cls->name->data,
                    cls->param_count, cls->param_count == 1 ? "" : "s", positional);
    }
    /* Which argument fills which field. */
    const LzValue *given[LZ_CLASS_FIELDS_MAX];
    for (size_t i = 0; i < cls->param_count; i++) {
        given[i] = i < positional ? &argv[i] : NULL;
    }
    for (size_t k = 0; k < named_count; k++) {
        LzString *name = NULL;
        if (name_at(vm, names, names_length, k, &name) == NULL) {
            return lz_vm_out_of_memory(vm);
        }
        size_t param = 0;
        LzStatus status = param_of(vm, cls, name, &param, cls->name->data);
        if (status != LZ_OK) {
            return status;
        }
        if (given[param] != NULL) {
            return fail(vm, LZ_ERR_ARITY, NULL, "'%s' was given twice", name->data);
        }
        given[param] = &named[k];
    }
    for (size_t i = 0; i < cls->param_count; i++) {
        const LzMember *member = &cls->members[cls->params[i]];
        if (given[i] == NULL && member->thunk.type == LZ_VAL_NIL) {
            return fail(vm, LZ_ERR_ARITY, NULL, "%s needs '%s'", cls->name->data, member->name->data);
        }
    }

    LzInstance *instance = instance_alloc(vm, cls);
    if (instance == NULL) {
        return lz_vm_out_of_memory(vm);
    }
    LzStatus status = LZ_OK;
    for (size_t i = 0; i < cls->param_count && status == LZ_OK; i++) {
        if (given[i] != NULL) {
            status = lz_instance_store(vm, instance, &cls->members[cls->params[i]], given[i]);
        }
    }
    /* The rest from their defaults, in declaration order. */
    for (size_t i = 0; i < cls->member_count && status == LZ_OK; i++) {
        const LzMember *member = &cls->members[i];
        if (member->kind != LZ_MEMBER_FIELD || ((instance->unset >> member->slot) & 1u) == 0 ||
            member->thunk.type == LZ_VAL_NIL) {
            continue;
        }
        LzValue value = lz_value_nil();
        status = call_closure(vm, &member->thunk, 0, NULL, &value);
        if (status == LZ_OK) {
            status = lz_instance_store(vm, instance, member, &value);
            lz_value_release(&vm->heap, &value);
        }
    }
    if (status == LZ_OK) {
        status = finish_instance(vm, instance);
    }
    if (status != LZ_OK) {
        lz_object_release(&vm->heap, &instance->base);
        return status;
    }
    *result = lz_value_object(&instance->base);
    return LZ_OK;
}

LzStatus lz_instance_copy(LzVM *vm, const LzInstance *source, LzValue *result) {
    LzInstance *copy = instance_alloc(vm, source->cls);
    if (copy == NULL) {
        return lz_vm_out_of_memory(vm);
    }
    for (size_t i = 0; i < source->cls->field_count; i++) {
        copy->fields[i] = source->fields[i];
        lz_value_retain(&copy->fields[i]);
        lz_note_container_store(&vm->heap, &copy->fields[i]);
    }
    copy->unset = source->unset;
    *result = lz_value_object(&copy->base);
    return LZ_OK;
}

LzStatus lz_instance_with(LzVM *vm, const LzInstance *source, size_t named_count, const char *names,
                          size_t names_length, const LzValue *named, LzValue *result) {
    LzClass *cls = source->cls;
    LzValue made = lz_value_nil();
    LzStatus status = lz_instance_copy(vm, source, &made);
    if (status != LZ_OK) {
        return status;
    }
    LzInstance *copy = lz_value_as_instance(&made);
    bool seen[LZ_CLASS_FIELDS_MAX] = {false};
    for (size_t k = 0; k < named_count && status == LZ_OK; k++) {
        LzString *name = NULL;
        if (name_at(vm, names, names_length, k, &name) == NULL) {
            status = lz_vm_out_of_memory(vm);
            break;
        }
        size_t param = 0;
        status = param_of(vm, cls, name, &param, cls->name->data);
        if (status != LZ_OK) {
            break;
        }
        if (seen[param]) {
            status = fail(vm, LZ_ERR_ARITY, NULL, "'%s' was given twice", name->data);
            break;
        }
        seen[param] = true;
        status = lz_instance_store(vm, copy, &cls->members[cls->params[param]], &named[k]);
    }
    if (status == LZ_OK) {
        status = finish_instance(vm, copy);
    }
    if (status != LZ_OK) {
        lz_object_release(&vm->heap, &copy->base);
        return status;
    }
    *result = made;
    return LZ_OK;
}

static LzStatus native_copy_fn(LzVM *vm, int argc, const LzValue *argv, LzValue *result) {
    (void)argc;
    if (!lz_value_is_instance(&argv[0])) {
        return lz_vm_set_error(vm, LZ_ERR_RUNTIME, "copy() needs an object");
    }
    return lz_instance_copy(vm, lz_value_as_instance(&argv[0]), result);
}

static LzStatus native_with_fn(LzVM *vm, int argc, const LzValue *argv, LzValue *result) {
    (void)argc;
    (void)argv;
    (void)result;
    return fail(vm, LZ_ERR_ARITY, NULL, "with() takes named fields, like p.with(x: 10)");
}

static LzObject *make_native(LzVM *vm, const char *name, LzNativeFn fn, int max_args) {
    LzString *interned = lz_vm_intern(vm, name, strlen(name));
    if (interned == NULL) {
        return NULL;
    }
    LzNative *native = lz_native_new(&vm->heap, interned, fn, 1, max_args);
    return native == NULL ? NULL : &native->base;
}

/* ---- member access ---- */

static LzStatus read_field(LzVM *vm, const LzInstance *instance, const LzMember *member, LzValue *out) {
    if (((instance->unset >> member->slot) & 1u) != 0) {
        return unset_error(vm, instance->cls, member);
    }
    *out = instance->fields[member->slot];
    lz_value_retain(out);
    return LZ_OK;
}

static LzStatus instance_get(LzVM *vm, const LzString *owner, LzInstance *instance,
                             const LzString *name, LzValue *result, bool for_write) {
    LzClass *cls = instance->cls;
    const LzMember *member = lz_class_find(cls, name);
    if (member == NULL) {
        return no_member(vm, cls, owner, name, "field", true, false);
    }
    if (!member_visible(owner, cls, member)) {
        return private_error(vm, cls, member);
    }
    switch (member->kind) {
    case LZ_MEMBER_FIELD:
        if (for_write && member->vis == LZ_VIS_PUB_READ && !lz_class_sees(owner, cls)) {
            return read_only_error(vm, cls, member);
        }
        return read_field(vm, instance, member, result);
    case LZ_MEMBER_GETTER: {
        LzValue self = lz_value_object(&instance->base);
        return call_closure(vm, &member->value, 1, &self, result);
    }
    case LZ_MEMBER_METHOD: {
        LzValue self = lz_value_object(&instance->base);
        LzBound *bound = lz_bound_new(&vm->heap, &member->value, &self, false);
        if (bound == NULL) {
            return lz_vm_out_of_memory(vm);
        }
        *result = lz_value_object(&bound->base);
        return LZ_OK;
    }
    case LZ_MEMBER_STATIC:
        return fail(vm, LZ_ERR_RUNTIME, NULL, "'%s' is a static function; call it as %s.%s()",
                    name->data, cls->name->data, name->data);
    case LZ_MEMBER_CONST:
        return fail(vm, LZ_ERR_RUNTIME, NULL, "'%s' is a constant of %s; write %s.%s", name->data,
                    cls->name->data, cls->name->data, name->data);
    }
    return LZ_OK;
}

static LzStatus class_get(LzVM *vm, const LzString *owner, LzClass *cls, const LzString *name,
                          LzValue *result) {
    const LzMember *member = lz_class_find(cls, name);
    if (member == NULL) {
        return no_member(vm, cls, owner, name, "constant or function", false, false);
    }
    if (!member_visible(owner, cls, member)) {
        return private_error(vm, cls, member);
    }
    switch (member->kind) {
    case LZ_MEMBER_CONST:
    case LZ_MEMBER_STATIC:
    case LZ_MEMBER_METHOD:
        *result = member->value;
        lz_value_retain(result);
        return LZ_OK;
    case LZ_MEMBER_FIELD:
    case LZ_MEMBER_GETTER:
        return fail(vm, LZ_ERR_RUNTIME, NULL, "'%s' belongs to an object of %s, not to the class itself",
                    name->data, cls->name->data);
    }
    return LZ_OK;
}

LzStatus lz_vm_member_get(LzVM *vm, const LzString *owner, const LzValue *target,
                          const LzString *name, LzValue *result, bool for_write) {
    if (lz_value_is_instance(target)) {
        return instance_get(vm, owner, lz_value_as_instance(target), name, result, for_write);
    }
    return class_get(vm, owner, lz_value_as_class(target), name, result);
}

LzStatus lz_vm_member_set(LzVM *vm, const LzString *owner, const LzValue *target,
                          const LzString *name, const LzValue *value) {
    if (lz_value_is_class(target)) {
        LzClass *cls = lz_value_as_class(target);
        const LzMember *member = lz_class_find(cls, name);
        if (member != NULL && member->kind == LZ_MEMBER_CONST) {
            return fail(vm, LZ_ERR_RUNTIME, NULL, "'%s' is a constant and cannot be assigned", name->data);
        }
        if (member == NULL) {
            return no_member(vm, cls, owner, name, "constant or function", false, true);
        }
        return fail(vm, LZ_ERR_RUNTIME, NULL, "'%s' of %s cannot be assigned", name->data, cls->name->data);
    }
    LzInstance *instance = lz_value_as_instance(target);
    LzClass *cls = instance->cls;
    const LzMember *member = lz_class_find(cls, name);
    if (member == NULL) {
        return no_member(vm, cls, owner, name, "field", true, true);
    }
    if (!member_visible(owner, cls, member)) {
        return private_error(vm, cls, member);
    }
    switch (member->kind) {
    case LZ_MEMBER_FIELD:
        if (member->vis == LZ_VIS_PUB_READ && !lz_class_sees(owner, cls)) {
            return read_only_error(vm, cls, member);
        }
        return lz_instance_store(vm, instance, member, value);
    case LZ_MEMBER_GETTER:
        return fail(vm, LZ_ERR_RUNTIME, NULL, "'%s' is a computed property and cannot be assigned",
                    name->data);
    case LZ_MEMBER_CONST:
        return fail(vm, LZ_ERR_RUNTIME, NULL, "'%s' is a constant and cannot be assigned", name->data);
    case LZ_MEMBER_METHOD:
    case LZ_MEMBER_STATIC:
        return fail(vm, LZ_ERR_RUNTIME, NULL, "'%s' is a function and cannot be assigned", name->data);
    }
    return LZ_OK;
}

/* Replaces the fallback on top of the stack by `callee` (a new reference, consumed). */
static LzStatus set_callee(LzVM *vm, size_t index, LzValue callee) {
    LzValue old = vm->stack[index];
    vm->stack[index] = callee;
    lz_value_release(&vm->heap, &old);
    return LZ_OK;
}

static LzStatus set_bound_callee(LzVM *vm, size_t index, const LzValue *callee, const LzValue *receiver,
                                 bool skip_first) {
    LzBound *bound = lz_bound_new(&vm->heap, callee, receiver, skip_first);
    if (bound == NULL) {
        return lz_vm_out_of_memory(vm);
    }
    return set_callee(vm, index, lz_value_object(&bound->base));
}

static LzStatus instance_callee(LzVM *vm, const LzString *owner, const LzString *name, int mode,
                                size_t receiver_index) {
    LzValue receiver = vm->stack[receiver_index];
    LzInstance *instance = lz_value_as_instance(&receiver);
    LzClass *cls = instance->cls;
    size_t fallback = receiver_index + 1;
    const LzMember *member = lz_class_find(cls, name);
    if (member != NULL && cls->init_member >= 0 && &cls->members[cls->init_member] == member) {
        return fail(vm, LZ_ERR_RUNTIME, NULL, "init() runs automatically when an object is created");
    }
    if (member != NULL) {
        if (!member_visible(owner, cls, member)) {
            return private_error(vm, cls, member);
        }
        switch (member->kind) {
        case LZ_MEMBER_METHOD:
            if (cls->init_member >= 0 && &cls->members[cls->init_member] == member) {
                return fail(vm, LZ_ERR_RUNTIME, NULL, "init() runs automatically when an object is created");
            }
            lz_value_retain(&member->value);
            return set_callee(vm, fallback, member->value);
        case LZ_MEMBER_STATIC:
            return fail(vm, LZ_ERR_RUNTIME, NULL, "'%s' is a static function; call it as %s.%s()",
                        name->data, cls->name->data, name->data);
        case LZ_MEMBER_GETTER:
            return fail(vm, LZ_ERR_RUNTIME, NULL, "'%s' is a property; write r.%s without parentheses",
                        name->data, name->data);
        case LZ_MEMBER_FIELD: {
            LzValue held = lz_value_nil();
            LzStatus status = read_field(vm, instance, member, &held);
            if (status != LZ_OK) {
                return status;
            }
            status = set_bound_callee(vm, fallback, &held, NULL, true);
            lz_value_release(&vm->heap, &held);
            return status;
        }
        case LZ_MEMBER_CONST:
            return fail(vm, LZ_ERR_RUNTIME, NULL, "'%s' is a constant of %s; write %s.%s", name->data,
                        cls->name->data, cls->name->data, name->data);
        }
    }
    /* Not a member: with() and copy() belong to every instance, then the builtin methods. */
    bool is_with = strcmp(name->data, "with") == 0;
    if (is_with || strcmp(name->data, "copy") == 0) {
        LzObject **slot = is_with ? &vm->native_with : &vm->native_copy;
        if (*slot == NULL) {
            *slot = is_with ? make_native(vm, "with", native_with_fn, 1)
                            : make_native(vm, "copy", native_copy_fn, 1);
            if (*slot == NULL) {
                return lz_vm_out_of_memory(vm);
            }
        }
        lz_object_retain(*slot);
        return set_callee(vm, fallback, lz_value_object(*slot));
    }
    if (strcmp(name->data, "init") == 0 && cls->init_member >= 0) {
        return fail(vm, LZ_ERR_RUNTIME, NULL, "init() runs automatically when an object is created");
    }
    if (mode == 0) {
        return LZ_OK; /* the fallback compiled with the call: a builtin method or a function of that name */
    }
    const LzValue *found = lz_table_get(&vm->methods, name);
    if (found == NULL) {
        found = lz_vm_lookup(vm, name);
    }
    if (found == NULL) {
        char suggestion[160];
        did_you_mean(cls, owner, name, false, suggestion, sizeof suggestion);
        return fail(vm, LZ_ERR_RUNTIME, NULL, "%s has no method or field '%s'%s", cls->name->data,
                    name->data, suggestion);
    }
    LzValue callee = *found;
    lz_value_retain(&callee);
    return set_callee(vm, fallback, callee);
}

static LzStatus class_callee(LzVM *vm, const LzString *owner, const LzString *name, size_t receiver_index) {
    LzValue receiver = vm->stack[receiver_index];
    LzClass *cls = lz_value_as_class(&receiver);
    const LzMember *member = lz_class_find(cls, name);
    if (member == NULL) {
        return no_member(vm, cls, owner, name, "function or constant", false, false);
    }
    if (!member_visible(owner, cls, member)) {
        return private_error(vm, cls, member);
    }
    switch (member->kind) {
    case LZ_MEMBER_STATIC:
    case LZ_MEMBER_METHOD:
    case LZ_MEMBER_CONST:
        /* The call passes the class as the first argument; the bound callee drops it again. */
        return set_bound_callee(vm, receiver_index + 1, &member->value, NULL, true);
    case LZ_MEMBER_FIELD:
    case LZ_MEMBER_GETTER:
        return fail(vm, LZ_ERR_RUNTIME, NULL, "'%s' belongs to an object of %s, not to the class itself",
                    name->data, cls->name->data);
    }
    return LZ_OK;
}

LzStatus lz_vm_method_lookup(LzVM *vm, const LzString *owner, const LzString *name, int mode,
                             size_t receiver_index) {
    const LzValue *receiver = &vm->stack[receiver_index];
    if (lz_value_is_instance(receiver)) {
        return instance_callee(vm, owner, name, mode, receiver_index);
    }
    if (lz_value_is_class(receiver)) {
        return class_callee(vm, owner, name, receiver_index);
    }
    if (mode == 0) {
        return LZ_OK;
    }
    const LzValue *found = lz_table_get(&vm->methods, name);
    if (found == NULL) {
        found = lz_vm_lookup(vm, name);
    }
    if (found == NULL) {
        return lz_vm_set_error(vm, LZ_ERR_RUNTIME, "undefined variable '%s'", name->data);
    }
    LzValue callee = *found;
    lz_value_retain(&callee);
    return set_callee(vm, receiver_index + 1, callee);
}

/* ---- text ---- */

#define WRITE_DEPTH_MAX 2000
static unsigned write_depth = 0;

static const LzMember *user_text_method(const LzClass *cls, const char *name) {
    for (size_t i = 0; i < cls->member_count; i++) {
        const LzMember *member = &cls->members[i];
        if (member->kind == LZ_MEMBER_METHOD && member->vis != LZ_VIS_PRIVATE &&
            strcmp(member->name->data, name) == 0) {
            return member;
        }
    }
    return NULL;
}

bool lz_instance_write(const LzInstance *instance, const LzWriter *writer, bool debug) {
    LzClass *cls = instance->cls;
    LzVM *vm = cls->vm;
    if (write_depth >= WRITE_DEPTH_MAX) {
        vm->text_status = lz_vm_set_error(vm, LZ_ERR_RUNTIME, "value is nested too deeply to print");
        return false;
    }
    write_depth++;
    bool ok = true;
    const LzMember *user = user_text_method(cls, debug ? "to_debug" : "to_string");
    if (user != NULL && vm->frame_count > 0) {
        LzValue self = lz_value_object((LzObject *)&instance->base);
        LzValue text = lz_value_nil();
        LzStatus status = call_closure(vm, &user->value, 1, &self, &text);
        if (status != LZ_OK) {
            vm->text_status = status;
            ok = false;
        } else if (text.type != LZ_VAL_OBJECT || text.as.object->type != LZ_OBJ_STRING) {
            vm->text_status = lz_vm_set_error(vm, LZ_ERR_TYPE, "%s() of %s must return a string, got %s",
                                              debug ? "to_debug" : "to_string", cls->name->data,
                                              lz_value_kind_name(&text));
            lz_value_release(&vm->heap, &text);
            ok = false;
        } else {
            const LzString *string = (const LzString *)text.as.object;
            ok = lz_writer_write(writer, string->data, string->length);
            lz_value_release(&vm->heap, &text);
        }
        write_depth--;
        return ok;
    }
    ok = lz_writer_write(writer, cls->name->data, cls->name->length) && lz_writer_write_cstr(writer, "(");
    bool first = true;
    for (size_t i = 0; ok && i < cls->member_count; i++) {
        const LzMember *member = &cls->members[i];
        if (member->kind != LZ_MEMBER_FIELD) {
            continue;
        }
        if (!first) {
            ok = lz_writer_write_cstr(writer, ", ");
        }
        first = false;
        ok = ok && lz_writer_write(writer, member->name->data, member->name->length) &&
             lz_writer_write_cstr(writer, ": ");
        if (!ok) {
            break;
        }
        if (((instance->unset >> member->slot) & 1u) != 0) {
            ok = lz_writer_write_cstr(writer, "<unset>");
        } else if (lz_value_is_instance(&instance->fields[member->slot])) {
            ok = lz_instance_write(lz_value_as_instance(&instance->fields[member->slot]), writer, true);
        } else {
            ok = lz_value_write_debug(&instance->fields[member->slot], writer);
        }
    }
    ok = ok && lz_writer_write_cstr(writer, ")");
    write_depth--;
    return ok;
}

bool lz_instance_same_class(const LzInstance *a, const LzInstance *b) {
    return a->cls == b->cls;
}

bool lz_values_identical(const LzValue *a, const LzValue *b) {
    if (a->type == LZ_VAL_OBJECT && b->type == LZ_VAL_OBJECT) {
        bool both_strings = a->as.object->type == LZ_OBJ_STRING && b->as.object->type == LZ_OBJ_STRING;
        if (!both_strings) {
            return a->as.object == b->as.object;
        }
    }
    return lz_value_equal(a, b);
}
