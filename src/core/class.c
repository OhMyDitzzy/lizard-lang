#include "lizard/class.h"
#include "lizard/source.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lizard/array.h"
#include "lizard/dict.h"
#include "lizard/error.h"
#include "lizard/link.h"
#include "lizard/native.h"
#include "lizard/suggest.h"

/* The classes of a VM sorted by id. Ids only grow, so adding is appending. */
static bool registry_add(LzVM *vm, LzClass *cls) {
    if (vm->class_registry_count == vm->class_registry_capacity) {
        size_t capacity = vm->class_registry_capacity == 0 ? 16 : vm->class_registry_capacity * 2;
        LzClass **grown = realloc(vm->class_registry, capacity * sizeof *grown);
        if (grown == NULL) {
            return false;
        }
        vm->class_registry = grown;
        vm->class_registry_capacity = capacity;
    }
    vm->class_registry[vm->class_registry_count++] = cls;
    return true;
}

static size_t registry_find(const LzVM *vm, uint64_t id) {
    size_t low = 0;
    size_t high = vm->class_registry_count;
    while (low < high) {
        size_t mid = low + (high - low) / 2;
        if (vm->class_registry[mid]->id < id) {
            low = mid + 1;
        } else {
            high = mid;
        }
    }
    return low;
}

static void registry_remove(LzVM *vm, const LzClass *cls) {
    size_t at = registry_find(vm, cls->id);
    if (at < vm->class_registry_count && vm->class_registry[at] == cls) {
        memmove(&vm->class_registry[at], &vm->class_registry[at + 1],
                (vm->class_registry_count - at - 1) * sizeof *vm->class_registry);
        vm->class_registry_count--;
    }
}

const LzClass *lz_class_by_id(const LzVM *vm, uint64_t id) {
    size_t at = registry_find(vm, id);
    return at < vm->class_registry_count && vm->class_registry[at]->id == id ? vm->class_registry[at] : NULL;
}

/* Is `trait`, or a trait it extends, the one with this id? */
static bool trait_extends(const LzClass *trait, uint64_t id) {
    if (trait->id == id) {
        return true;
    }
    for (size_t i = 0; i < trait->trait_count; i++) {
        if (trait_extends(trait->traits[i], id)) {
            return true;
        }
    }
    return false;
}

bool lz_class_is_a(const LzClass *cls, uint64_t id) {
    for (; cls != NULL; cls = cls->parent) {
        if (cls->id == id) {
            return true;
        }
        for (size_t i = 0; i < cls->trait_count; i++) {
            if (trait_extends(cls->traits[i], id)) {
                return true;
            }
        }
    }
    return false;
}

LzClass *lz_class_new(LzVM *vm, LzString *name, uint32_t offset) {
    LzClass *cls = lz_heap_alloc(&vm->heap, LZ_OBJ_CLASS, sizeof *cls);
    if (cls == NULL) {
        return NULL;
    }
    cls->id = ++vm->next_class_id;
    cls->name = name;
    lz_object_retain(&name->base);
    cls->parent = NULL;
    cls->is_trait = false;
    cls->traits = NULL;
    cls->trait_count = 0;
    cls->linked = false;
    cls->own_field_count = 0;
    cls->vm = vm;
    cls->members = NULL;
    cls->member_count = 0;
    cls->member_capacity = 0;
    cls->field_count = 0;
    cls->param_count = 0;
    cls->init_member = -1;
    cls->consts_ready = false;
    cls->offset = offset;
    cls->is_ext = false;
    cls->ext_kinds = 0;
    cls->ext_target_id = 0;
    cls->ext_error_kind = 0;
    cls->ext_target = NULL;
    if (!registry_add(vm, cls)) {
        lz_object_release(&vm->heap, &cls->base); /* the finalizer drops the name it holds */
        return NULL;
    }
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

/*
 * Fields by slot and constructor parameters, the parent's and then this class's own. The slots
 * of the own fields start where the parent's end (`base`), and a class with no parent has base
 * 0.
 */
static void layout(LzClass *cls, size_t base) { /* `base` is added to the own slots */
    const LzClass *parent = cls->parent;
    cls->param_count = 0;
    if (parent != NULL) {
        for (size_t i = 0; i < parent->field_count; i++) {
            cls->field_refs[i] = parent->field_refs[i];
        }
        for (size_t i = 0; i < parent->param_count; i++) {
            cls->params[cls->param_count++] = parent->params[i];
        }
    }
    for (size_t i = 0; i < cls->member_count; i++) {
        LzMember *member = &cls->members[i];
        if (member->kind != LZ_MEMBER_FIELD) {
            continue;
        }
        member->slot = (uint16_t)(member->slot + base);
        cls->field_refs[member->slot].cls = cls;
        cls->field_refs[member->slot].index = (uint16_t)i;
        if (member->vis != LZ_VIS_PRIVATE) {
            cls->params[cls->param_count++] = member->slot;
        }
    }
    cls->field_count = base + cls->own_field_count;
}

void lz_class_seal(LzClass *cls) {
    cls->init_member = -1;
    cls->own_field_count = cls->field_count;
    for (size_t i = 0; i < cls->member_count; i++) {
        const LzMember *member = &cls->members[i];
        if (member->kind == LZ_MEMBER_METHOD && strcmp(member->name->data, "init") == 0) {
            cls->init_member = (int)i;
        }
    }
    layout(cls, 0); /* as a class without a parent, and lz_class_link redoes it for one with a parent */
}

/* The member of a field slot. */
static const LzMember *field_at(const LzClass *cls, size_t slot) {
    const LzFieldRef *ref = &cls->field_refs[slot];
    return &ref->cls->members[ref->index];
}

/*
 * A member of a trait, or of a trait it extends. `required` picks the requirements, which have
 * no body, and otherwise the defaults. A trait's own member is found before the ones of the
 * traits it extends.
 */
static bool trait_find(const LzClass *trait, const LzString *name, bool required, LzFound *out) {
    for (size_t i = 0; i < trait->member_count; i++) {
        const LzMember *member = &trait->members[i];
        if (member->name == name && (member->is_required != 0) == required) {
            out->member = member;
            out->declared_in = trait;
            return true;
        }
    }
    for (size_t i = 0; i < trait->trait_count; i++) {
        if (trait_find(trait->traits[i], name, required, out)) {
            return true;
        }
    }
    return false;
}

/*
 * The classes of the chain win, and the default of a trait answers only when none of them has
 * the name.
 */
bool lz_class_lookup(const LzClass *cls, const LzString *name, LzFound *out) {
    for (const LzClass *c = cls; c != NULL; c = c->parent) {
        for (size_t i = 0; i < c->member_count; i++) {
            if (c->members[i].name == name && c->members[i].is_required == 0) {
                out->member = &c->members[i];
                out->declared_in = c;
                return true;
            }
        }
    }
    /*
     * Then comes what `extend` added to the class or to an ancestor, before the defaults of
     * traits.
     */
    if (cls->vm != NULL && cls->vm->ext_count != 0 && lz_ext_find_class(cls->vm, cls, name, out)) {
        return true;
    }
    for (const LzClass *c = cls; c != NULL; c = c->parent) {
        for (size_t i = 0; i < c->trait_count; i++) {
            if (trait_find(c->traits[i], name, false, out)) {
                return true;
            }
        }
    }
    return false;
}

const LzMember *lz_class_find(const LzClass *cls, const LzString *name) {
    for (size_t i = 0; i < cls->member_count; i++) {
        if (cls->members[i].name == name) {
            return &cls->members[i];
        }
    }
    return NULL;
}

bool lz_class_sees(const LzVM *vm, uint64_t owner, const LzClass *declaring) {
    if (owner == 0) {
        return false;
    }
    if (owner == declaring->id) {
        return true;
    }
    const LzClass *code = lz_class_by_id(vm, owner);
    return code != NULL && lz_class_is_a(code, declaring->id);
}

const char *lz_class_display_name(const LzClass *cls) {
    return cls->name->data;
}

void lz_instance_drop_holder(const LzValue *item) {
    lz_value_as_instance(item)->holders--;
}

void lz_class_finalize(LzHeap *heap, LzClass *cls) {
    for (size_t i = 0; i < cls->member_count; i++) {
        LzMember *member = &cls->members[i];
        lz_value_release(heap, &member->value);
        lz_value_release(heap, &member->thunk);
        lz_object_release(heap, &member->name->base);
    }
    free(cls->members);
    cls->members = NULL;
    registry_remove(cls->vm, cls);
    for (size_t i = 0; i < cls->trait_count; i++) {
        lz_object_release(heap, &cls->traits[i]->base);
    }
    free(cls->traits);
    cls->traits = NULL;
    cls->trait_count = 0;
    if (cls->parent != NULL) {
        lz_object_release(heap, &cls->parent->base);
        cls->parent = NULL;
    }
    if (cls->ext_target != NULL) {
        lz_object_release(heap, &cls->ext_target->base);
        cls->ext_target = NULL;
    }
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

/* `declared` is the class that declares the member, not the class of the object. */
static bool member_visible(uint64_t owner, const LzClass *declared, const LzMember *member) {
    return member->vis != LZ_VIS_PRIVATE || lz_class_sees(declared->vm, owner, declared);
}

/* " (did you mean 'x'?)" for a name that is not a member, from the members the caller can see. */
static void did_you_mean(const LzClass *cls, uint64_t owner, const LzString *name,
                         bool fields_only, char *out, size_t size) {
    out[0] = '\0';
    LzSuggester suggester;
    lz_suggester_init(&suggester, name->data, name->length);
    for (const LzClass *c = cls; c != NULL; c = c->parent) {
        for (size_t i = 0; i < c->member_count; i++) {
            const LzMember *member = &c->members[i];
            if (fields_only && member->kind != LZ_MEMBER_FIELD) {
                continue;
            }
            if (member_visible(owner, c, member)) {
                lz_suggester_offer(&suggester, member->name->data);
            }
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

static LzStatus no_member(LzVM *vm, const LzClass *cls, uint64_t owner, const LzString *name,
                          const char *what, bool fields_only, bool write) {
    char suggestion[160];
    did_you_mean(cls, owner, name, fields_only, suggestion, sizeof suggestion);
    return fail(vm, LZ_ERR_RUNTIME, write ? "fields are declared in the class" : NULL,
                "%s has no %s '%s'%s", cls->name->data, what, name->data, suggestion);
}

static uint64_t calling_owner(const LzVM *vm);

static LzInstance *payload_of(const LzValue *value);

static LzStatus private_error(LzVM *vm, const LzClass *declared, const LzMember *member) {
    uint64_t caller = calling_owner(vm);
    const LzClass *code = caller != 0 ? lz_class_by_id(vm, caller) : NULL;
    if (code != NULL && code->is_ext && !declared->is_ext) {
        char help[160];
        (void)snprintf(help, sizeof help, "an extension can only use the public members of %s", declared->name->data);
        return fail(vm, LZ_ERR_RUNTIME, help, "'%s' is private to %s", member->name->data, declared->name->data);
    }
    if (declared->is_ext) {
        return fail(vm, LZ_ERR_RUNTIME, "it can only be used inside the extend block that declares it",
                    "'%s' is private to its extend %s block", member->name->data, declared->name->data);
    }
    const char *help = member->kind == LZ_MEMBER_FIELD || member->kind == LZ_MEMBER_GETTER
                           ? "mark it 'pub' (or 'pub read') to use it from outside"
                           : (member->kind == LZ_MEMBER_CONST ? "mark it 'pub' to use it from outside"
                                                              : "mark it 'pub' to call it from outside");
    if (member->kind == LZ_MEMBER_GETTER) {
        help = "mark it 'pub' to use it from outside";
    }
    return fail(vm, LZ_ERR_RUNTIME, help, "'%s' is private to %s", member->name->data, declared->name->data);
}

static LzStatus read_only_error(LzVM *vm, const LzClass *cls, const LzMember *member) { /* cls: the class that declares it */
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
    return fail(vm, LZ_ERR_RUNTIME,
                "give the field a default (pub x = 0) or pass it when you create the object",
                "field '%s' of %s has no value yet (assign it in init())", member->name->data,
                cls->name->data);
}

static const char *kind_phrase(const LzMember *member) {
    switch ((LzMemberKind)member->kind) {
    case LZ_MEMBER_FIELD:
        return "a field";
    case LZ_MEMBER_METHOD:
        return "a function";
    case LZ_MEMBER_STATIC:
        return "a static function";
    case LZ_MEMBER_GETTER:
        return "a property";
    case LZ_MEMBER_CONST:
        return "a constant";
    }
    return "a member";
}

/* "Dog.speak()" for a function, "Dog.area" for a property or anything else. */
static void member_label(char *out, size_t size, const LzClass *cls, const LzMember *member) {
    bool call = member->kind == LZ_MEMBER_METHOD || member->kind == LZ_MEMBER_STATIC;
    (void)snprintf(out, size, "%s.%s%s", cls->name->data, member->name->data, call ? "()" : "");
}

static bool is_function_like(const LzMember *member) {
    return member->kind == LZ_MEMBER_METHOD || member->kind == LZ_MEMBER_STATIC || member->kind == LZ_MEMBER_GETTER;
}

/* Parameters a caller writes, and `self` is not one. */
static size_t written_params(const LzMember *member) {
    if (member->value.type != LZ_VAL_OBJECT || member->value.as.object->type != LZ_OBJ_CLOSURE) {
        return 0;
    }
    size_t count = lz_value_as_closure(&member->value)->function->param_count;
    bool has_self = member->kind == LZ_MEMBER_METHOD || member->kind == LZ_MEMBER_GETTER;
    return has_self && count > 0 ? count - 1 : count;
}

/*
 * An error placed on the declaration that is wrong, because the run-time error points at its
 * source.
 */
static LzStatus link_fail(LzVM *vm, uint32_t offset, const char *help, const char *format, ...)
    LZ_PRINTF_FORMAT(4, 5);
static LzStatus link_fail(LzVM *vm, uint32_t offset, const char *help, const char *format, ...) {
    char message[256];
    va_list args;
    va_start(args, format);
    (void)vsnprintf(message, sizeof message, format, args);
    va_end(args);
    LzStatus status = fail(vm, LZ_ERR_RUNTIME, help, "%s", message);
    vm->error_offset = offset;
    return status;
}

/*
 * The member `name` that a class inherits, if any. It comes from the parent chain and the
 * defaults of its traits, else from a default of a trait of this class, else from a requirement
 * of one when *required is set.
 */
static bool find_inherited(const LzClass *cls, const LzString *name, LzFound *out, bool *required) {
    *required = false;
    if (cls->parent != NULL && lz_class_lookup(cls->parent, name, out)) {
        return true;
    }
    for (size_t i = 0; i < cls->trait_count; i++) {
        if (trait_find(cls->traits[i], name, false, out)) {
            return true;
        }
    }
    for (size_t i = 0; i < cls->trait_count; i++) {
        if (trait_find(cls->traits[i], name, true, out)) {
            *required = true;
            return true;
        }
    }
    return false;
}

static void offer_trait_names(LzSuggester *suggester, const LzClass *trait) {
    for (size_t i = 0; i < trait->member_count; i++) {
        lz_suggester_offer(suggester, trait->members[i].name->data);
    }
    for (size_t i = 0; i < trait->trait_count; i++) {
        offer_trait_names(suggester, trait->traits[i]);
    }
}

static LzStatus check_overrides(LzVM *vm, const LzClass *cls) {
    for (size_t i = 0; i < cls->member_count; i++) {
        const LzMember *own = &cls->members[i];
        LzFound inherited;
        bool required = false;
        bool found = find_inherited(cls, own->name, &inherited, &required);
        if (!found) {
            if (own->is_override) {
                LzSuggester suggester;
                lz_suggester_init(&suggester, own->name->data, own->name->length);
                for (const LzClass *c = cls->parent; c != NULL; c = c->parent) {
                    for (size_t k = 0; k < c->member_count; k++) {
                        if (is_function_like(&c->members[k])) {
                            lz_suggester_offer(&suggester, c->members[k].name->data);
                        }
                    }
                    for (size_t k = 0; k < c->trait_count; k++) {
                        offer_trait_names(&suggester, c->traits[k]);
                    }
                }
                for (size_t k = 0; k < cls->trait_count; k++) {
                    offer_trait_names(&suggester, cls->traits[k]);
                }
                char label[128];
                char hint[96] = "";
                member_label(label, sizeof label, cls, own);
                if (suggester.found.count > 0) {
                    (void)snprintf(hint, sizeof hint, " (did you mean '%s'?)", suggester.found.names[0]);
                }
                return link_fail(vm, own->offset, "remove 'override', or fix the name", "%s overrides nothing%s",
                                 label, hint);
            }
            continue;
        }
        if (required) {
            continue; /* meeting a requirement: the contract check below judges it */
        }
        const LzMember *base = inherited.member;
        const LzClass *base_cls = inherited.declared_in;
        if (own->kind == LZ_MEMBER_FIELD && base->kind == LZ_MEMBER_FIELD) {
            return link_fail(vm, own->offset, "a field of the parent is already a field of this class",
                             "%s redeclares field '%s' of %s", cls->name->data, own->name->data,
                             base_cls->name->data);
        }
        if (own->kind == LZ_MEMBER_METHOD && base->kind == LZ_MEMBER_METHOD && strcmp(own->name->data, "init") == 0) {
            continue; /* every init runs, root first */
        }
        bool both_functions = is_function_like(own) && is_function_like(base);
        if (!both_functions) {
            return link_fail(vm, own->offset, NULL, "'%s' is %s of %s and cannot be redeclared as %s",
                             own->name->data, kind_phrase(base), base_cls->name->data, kind_phrase(own));
        }
        char own_label[128];
        char base_label[128];
        member_label(own_label, sizeof own_label, cls, own);
        member_label(base_label, sizeof base_label, base_cls, base);
        if (own->kind != base->kind) {
            return link_fail(vm, own->offset, NULL, "%s.%s is %s but %s.%s is %s", cls->name->data,
                             own->name->data, kind_phrase(own), base_cls->name->data, base->name->data,
                             kind_phrase(base));
        }
        if (!own->is_override) {
            return link_fail(vm, own->offset, "write 'override' before it if you mean to replace the parent's",
                             "%s hides %s; add 'override'", own_label, base_label);
        }
        size_t expected = written_params(base);
        size_t got = written_params(own);
        if (expected != got) {
            return link_fail(vm, own->offset, NULL,
                             "%s must take the same parameters as %s (expected %zu, got %zu)", own_label,
                             base_label, expected, got);
        }
        if (own->vis == LZ_VIS_PRIVATE && base->vis != LZ_VIS_PRIVATE) {
            return link_fail(vm, own->offset, "an override may be more open than the original, not less",
                             "%s cannot be private because %s is public", own_label, base_label);
        }
    }
    return LZ_OK;
}

LzStatus lz_class_add_trait(LzVM *vm, LzClass *cls, LzClass *trait) {
    for (size_t i = 0; i < cls->trait_count; i++) {
        if (cls->traits[i] == trait) {
            return link_fail(vm, cls->offset, "name each trait once", "trait '%s' is listed twice in the head of %s",
                             trait->name->data, cls->name->data);
        }
    }
    LzClass **grown = realloc(cls->traits, (cls->trait_count + 1) * sizeof *grown);
    if (grown == NULL) {
        return lz_vm_out_of_memory(vm);
    }
    cls->traits = grown;
    lz_object_retain(&trait->base);
    cls->traits[cls->trait_count++] = trait;
    return LZ_OK;
}

#define TRAIT_SET_MAX 64
typedef struct TraitSet {
    const LzClass *items[TRAIT_SET_MAX];
    size_t count;
} TraitSet;

/* `trait` and the traits it extends, each once. */
static void collect_trait(const LzClass *trait, TraitSet *set) {
    for (size_t i = 0; i < set->count; i++) {
        if (set->items[i] == trait) {
            return;
        }
    }
    if (set->count < TRAIT_SET_MAX) {
        set->items[set->count++] = trait;
    }
    for (size_t i = 0; i < trait->trait_count; i++) {
        collect_trait(trait->traits[i], set);
    }
}

static const LzMember *own_member(const LzClass *cls, const LzString *name) {
    for (size_t i = 0; i < cls->member_count; i++) {
        if (cls->members[i].name == name) {
            return &cls->members[i];
        }
    }
    return NULL;
}

/* A member of the class chain, not of a trait, with this name. */
static bool chain_has(const LzClass *cls, const LzString *name, LzFound *out) {
    for (const LzClass *c = cls; c != NULL; c = c->parent) {
        const LzMember *member = own_member(c, name);
        if (member != NULL) {
            out->member = member;
            out->declared_in = c;
            return true;
        }
    }
    return false;
}

static LzStatus check_contracts(LzVM *vm, const LzClass *cls, const TraitSet *all) {
    TraitSet mine = {.count = 0};
    for (size_t i = 0; i < cls->trait_count; i++) {
        collect_trait(cls->traits[i], &mine);
    }
    for (size_t t = 0; t < mine.count; t++) {
        const LzClass *trait = mine.items[t];
        for (size_t m = 0; m < trait->member_count; m++) {
            const LzMember *required = &trait->members[m];
            if (!required->is_required) {
                continue;
            }
            LzFound impl;
            if (!chain_has(cls, required->name, &impl)) {
                bool specialised = false; /* a trait that extends this one may carry the default */
                for (size_t q = 0; q < all->count && !specialised; q++) {
                    const LzMember *other = own_member(all->items[q], required->name);
                    specialised = other != NULL && !other->is_required && all->items[q] != trait &&
                                  trait_extends(all->items[q], trait->id);
                }
                if (specialised) {
                    continue;
                }
                return link_fail(vm, cls->offset, "write it in the class, or take it from a parent class",
                                 "%s does not implement '%s' required by %s", cls->name->data,
                                 required->name->data, trait->name->data);
            }
            const LzMember *given = impl.member;
            if (required->kind == LZ_MEMBER_GETTER && given->kind == LZ_MEMBER_FIELD) {
                return link_fail(vm, given->offset, "write it as: get name(self) => ...",
                                 "'%s' is required as a 'get' property; a field cannot satisfy it",
                                 required->name->data);
            }
            if (given->kind != required->kind) {
                return link_fail(vm, given->offset, NULL, "%s.%s is %s but %s.%s is %s", impl.declared_in->name->data,
                                 given->name->data, kind_phrase(given), trait->name->data, required->name->data,
                                 kind_phrase(required));
            }
            char given_label[128];
            char required_label[128];
            member_label(given_label, sizeof given_label, impl.declared_in, given);
            member_label(required_label, sizeof required_label, trait, required);
            size_t expected = written_params(required);
            size_t got = written_params(given);
            if (expected != got) {
                return link_fail(vm, given->offset, NULL,
                                 "%s must take the same parameters as %s (expected %zu, got %zu)", given_label,
                                 required_label, expected, got);
            }
            if (given->vis == LZ_VIS_PRIVATE) {
                return link_fail(vm, given->offset, "add 'pub'", "%s must be public because %s requires it",
                                 given_label, trait->name->data);
            }
        }
    }
    return LZ_OK;
}

/*
 * Two traits that give a class the same name, either as a default or as a default against a
 * requirement.
 */
static LzStatus check_conflicts(LzVM *vm, const LzClass *cls, const TraitSet *all) {
    for (size_t t = 0; t < all->count; t++) {
        const LzClass *trait = all->items[t];
        for (size_t m = 0; m < trait->member_count; m++) {
            const LzString *name = trait->members[m].name;
            /* look at a name once, at the first trait that has it */
            bool seen = false;
            for (size_t earlier = 0; earlier < t && !seen; earlier++) {
                seen = own_member(all->items[earlier], name) != NULL;
            }
            if (seen) {
                continue;
            }
            const LzClass *providers[TRAIT_SET_MAX];
            size_t count = 0;
            bool any_default = false;
            for (size_t q = 0; q < all->count; q++) {
                const LzMember *member = own_member(all->items[q], name);
                if (member != NULL) {
                    providers[count++] = all->items[q];
                    any_default = any_default || !member->is_required;
                }
            }
            /* a trait that extends another one wins over it */
            const LzClass *left[TRAIT_SET_MAX];
            size_t left_count = 0;
            for (size_t p = 0; p < count; p++) {
                bool beaten = false;
                for (size_t q = 0; q < count && !beaten; q++) {
                    beaten = q != p && trait_extends(providers[q], providers[p]->id);
                }
                if (!beaten) {
                    left[left_count++] = providers[p];
                }
            }
            LzFound defined;
            if (left_count < 2 || !any_default || chain_has(cls, name, &defined)) {
                continue;
            }
            return link_fail(vm, cls->offset, "define it in the class to say which one you mean",
                             "%s gets '%s' from both %s and %s; define %s() in %s to choose", cls->name->data,
                             name->data, left[0]->name->data, left[1]->name->data, name->data, cls->name->data);
        }
    }
    return LZ_OK;
}

LzStatus lz_class_finish(LzVM *vm, LzClass *cls) {
    if (cls->is_trait) {
        return LZ_OK;
    }
    LzStatus status = check_overrides(vm, cls);
    if (status != LZ_OK) {
        return status;
    }
    TraitSet all = {.count = 0};
    for (const LzClass *c = cls; c != NULL; c = c->parent) {
        for (size_t i = 0; i < c->trait_count; i++) {
            collect_trait(c->traits[i], &all);
        }
    }
    if (all.count == 0) {
        return LZ_OK;
    }
    status = check_conflicts(vm, cls, &all); /* a name two traits both give: say so before what is missing */
    if (status != LZ_OK) {
        return status;
    }
    return check_contracts(vm, cls, &all);
}

LzStatus lz_class_link(LzVM *vm, LzClass *cls, LzClass *parent) {
    if (cls->parent == parent) {
        return LZ_OK; /* the statement ran before */
    }
    if (parent->field_count + cls->own_field_count > LZ_CLASS_FIELDS_MAX) {
        return link_fail(vm, cls->offset, "a class and its parents can have at most 64 fields together",
                         "%s has too many fields with those of %s", cls->name->data, parent->name->data);
    }
    lz_object_retain(&parent->base);
    cls->parent = parent;
    layout(cls, parent->field_count);
    cls->linked = true;
    return LZ_OK;
}

/*
 * What `self` is for the code of the class, which is the instance or the error that carries it
 * as payload.
 */
static LzValue self_of(const LzInstance *instance) {
    return lz_value_object(instance->owner_error != NULL ? instance->owner_error : (LzObject *)&instance->base);
}

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
    instance->owner_error = NULL;
    instance->unset = cls->field_count >= 64 ? UINT64_MAX : (((uint64_t)1 << cls->field_count) - 1u);
    for (size_t i = 0; i < cls->field_count; i++) {
        instance->fields[i] = lz_value_nil();
    }
    return instance;
}

/*
 * What the value is, for "expected X, got Y". It's a type for containers and literals, else its
 * kind.
 */
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
    if (member->is_link && value->type == LZ_VAL_NIL) {
        return LZ_OK; /* a link field may always be nil, which means "points at nothing" */
    }
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
    LzStatus status = fail(vm, LZ_ERR_TYPE, NULL, "%s.%s expects %s, got %s%s", cls->name->data,
                           member->name->data, expected.data != NULL ? expected.data : "?",
                           actual.data != NULL ? actual.data : "?",
                           lz_value_is_link(value) ? "; use .target" : "");
    lz_buffer_free(&expected);
    lz_buffer_free(&actual);
    return status;
}

static LzStatus link_value_for(LzVM *vm, const LzValue *value, LzValue *cell, LzValue *made);

LzStatus lz_instance_store(LzVM *vm, LzInstance *instance, const LzMember *member,
                           const LzValue *value) {
    LzClass *cls = instance->cls;
    if (member->is_link) {
        /*
         * The type is checked against the target and never the cell, because links don't make
         * cycles.
         */
        LzValue cell = lz_value_nil();
        LzValue made = lz_value_nil();
        LzStatus status = link_value_for(vm, value, &cell, &made);
        if (status != LZ_OK) {
            return status;
        }
        LzValue target = lz_value_is_link(&cell) ? lz_link_deref(lz_value_as_link(&cell)) : lz_value_nil();
        status = check_field_type(vm, cls, member, &target);
        lz_value_release(&vm->heap, &target);
        if (status != LZ_OK) {
            lz_value_release(&vm->heap, &cell);
            lz_value_release(&vm->heap, &made);
            return status;
        }
        LzValue old = instance->fields[member->slot];
        instance->fields[member->slot] = cell;
        instance->unset &= ~((uint64_t)1 << member->slot);
        lz_value_release(&vm->heap, &old);
        lz_value_release(&vm->heap, &made); /* the field keeps its own reference */
        return LZ_OK;
    }
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
            return lz_vm_cycle_error(vm, instance, member, 'f');
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


/*
 * "pub link relation: Person = nil" for the field `member`, written the way it was declared.
 * That's the line of its declaration when the source is at hand, else built from what the class
 * knows.
 */
static void link_declaration(const LzVM *vm, const LzMember *member, char *out, size_t size) {
    const char *prefix = member->vis == LZ_VIS_PUB        ? "pub link "
                         : member->vis == LZ_VIS_PUB_READ ? "pub read link "
                                                          : "link ";
    const LzSource *source = vm->trace_source;
    if (source != NULL && member->offset < source->length &&
        strncmp(source->text + member->offset, member->name->data, member->name->length) == 0) {
        size_t end = member->offset;
        while (end < source->length && source->text[end] != '\n' && source->text[end] != '\r') {
            end++;
        }
        while (end > member->offset && (source->text[end - 1] == ' ' || source->text[end - 1] == '\t')) {
            end--;
        }
        (void)snprintf(out, size, "%s%.*s", prefix, (int)(end - member->offset),
                       source->text + member->offset);
        return;
    }
    LzBuffer type = {0};
    LzWriter type_out = lz_writer_from_buffer(&type);
    bool annotated = member->type.kinds != LZ_KINDS_ANY || member->type.literal_count > 0;
    if (annotated) {
        lz_type_describe(&member->type, &type_out);
    }
    (void)snprintf(out, size, "%s%s%s%s%s", prefix, member->name->data, annotated ? ": " : "",
                   annotated && type.data != NULL ? type.data : "",
                   member->thunk.type == LZ_VAL_NIL ? "" : " = ...");
    lz_buffer_free(&type);
}

LzStatus lz_vm_cycle_error(LzVM *vm, const LzInstance *instance, const LzMember *member, char kind) {
    (void)lz_vm_set_error(vm, LZ_ERR_RUNTIME, "cannot store: this would create a reference cycle");
    if (instance == NULL || member == NULL) {
        bool is_array = kind == 'a';
        vm->error_kind = (uint8_t)LZ_ERROR_VALUE;
        lz_vm_set_label(vm, "this %s would end up containing itself", is_array ? "array" : "dict");
        lz_vm_set_note(vm, "%s", is_array ? "an array owns its items, so it cannot end up inside itself"
                                          : "a dict owns its values, so it cannot end up inside itself");
        lz_vm_set_help(vm, is_array ? "store a link instead: items.push(link(x)), and read it back with .target"
                                    : "store a link instead: d[key] = link(x), and read it back with .target");
        return LZ_ERR_RUNTIME;
    }
    vm->error_kind = (uint8_t)LZ_ERROR_RUNTIME; /* the wording must not pick another kind */
    lz_vm_set_label(vm, "`%s.%s` would own an object that already owns this one",
                    instance->cls->name->data, member->name->data);
    if (instance->owner_error != NULL) {
        lz_vm_set_note(vm, "an error owns its fields, its `cause:` and its `data:`, so a cause cannot "
                           "lead back to the error that holds it");
    } else {
        lz_vm_set_note(vm, "an object owns what its fields hold, so two objects cannot own each other");
    }
    char declaration[256];
    link_declaration(vm, member, declaration, sizeof declaration);
    lz_vm_set_help(vm, "make the field a link: it points at an object without owning it\n%s", declaration);
    /*
     * The visual fix puts `link ` in front of the field's name. It's only done when the
     * declaration is known to be there, meaning the name is at the recorded offset, and never by
     * guessing.
     */
    const LzSource *source = vm->trace_source;
    bool placed = source == NULL || (member->offset < source->length &&
                                     strncmp(source->text + member->offset, member->name->data,
                                             member->name->length) == 0);
    if (placed && member->offset != LZ_NO_OFFSET) {
        lz_vm_set_fix(vm, "make the field a link: it points at an object without owning it",
                      member->offset, 0, "link ");
    }
    return LZ_ERR_RUNTIME;
}

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

/* The `init` of every class that has one, from the root of the hierarchy down to the leaf. */
static LzStatus run_inits(LzVM *vm, const LzClass *cls, const LzValue *self) {
    if (cls->parent != NULL) {
        LzStatus status = run_inits(vm, cls->parent, self);
        if (status != LZ_OK) {
            return status;
        }
    }
    if (cls->init_member >= 0) {
        LzValue ignored = lz_value_nil();
        LzStatus status = call_closure(vm, &cls->members[cls->init_member].value, 1, self, &ignored);
        if (status != LZ_OK) {
            return status;
        }
        lz_value_release(&vm->heap, &ignored);
    }
    return LZ_OK;
}

static LzStatus finish_instance(LzVM *vm, LzInstance *instance) {
    LzClass *cls = instance->cls;
    LzValue self = self_of(instance);
    LzStatus status = run_inits(vm, cls, &self);
    if (status != LZ_OK) {
        return status;
    }
    if (instance->unset != 0) {
        for (size_t slot = 0; slot < cls->field_count; slot++) {
            if (((instance->unset >> slot) & 1u) != 0) {
                return unset_error(vm, cls, field_at(cls, slot));
            }
        }
    }
    return LZ_OK;
}

/*
 * "signature: Person(name, relation)" is the constructor of `cls`, with an error's message
 * first.
 */
static void signature_help(const LzClass *cls, bool with_message, char *out, size_t size) {
    size_t used = (size_t)snprintf(out, size, "signature: %s(", cls->name->data);
    bool first = true;
    if (with_message && used < size) {
        used += (size_t)snprintf(out + used, size - used, "message");
        first = false;
    }
    for (size_t i = 0; i < cls->param_count && used < size; i++) {
        used += (size_t)snprintf(out + used, size - used, "%s%s", first ? "" : ", ",
                                 field_at(cls, cls->params[i])->name->data);
        first = false;
    }
    if (used < size) {
        (void)snprintf(out + used, size - used, ")");
    }
}

/*
 * Which parameter of the constructor is `name`, which must be a public field. Fails with the
 * right message.
 */
static LzStatus param_of(LzVM *vm, const LzClass *cls, const LzString *name, size_t *param,
                         const char *callname) {
    for (size_t i = 0; i < cls->param_count; i++) {
        if (field_at(cls, cls->params[i])->name == name) {
            *param = i;
            return LZ_OK;
        }
    }
    LzFound found;
    if (lz_class_lookup(cls, name, &found) && found.member->kind == LZ_MEMBER_FIELD) {
        return fail(vm, LZ_ERR_ARITY, NULL, "'%s' is private to %s, so it is not an argument of %s(...)",
                    name->data, cls->name->data, callname);
    }
    char suggestion[160];
    suggestion[0] = '\0';
    LzSuggester suggester;
    lz_suggester_init(&suggester, name->data, name->length);
    for (size_t i = 0; i < cls->param_count; i++) {
        lz_suggester_offer(&suggester, field_at(cls, cls->params[i])->name->data);
    }
    if (suggester.found.count > 0) {
        (void)snprintf(suggestion, sizeof suggestion, " (did you mean '%s'?)", suggester.found.names[0]);
    }
    char signature[256];
    signature_help(cls, false, signature, sizeof signature);
    return fail(vm, LZ_ERR_ARITY, signature, "%s has no field '%s'%s", cls->name->data, name->data,
                suggestion);
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

/* `core` is the error value of an error class, which takes the instance as its payload. */
static LzStatus construct_impl(LzVM *vm, LzClass *cls, size_t positional, const LzValue *argv,
                               size_t named_count, const char *names, size_t names_length,
                               const LzValue *named, LzError *core, LzValue *result) {
    if (cls->is_trait) {
        return fail(vm, LZ_ERR_RUNTIME, "a trait is implemented by a class; create one of those",
                    "%s is a trait and cannot be created; use a class that implements it", cls->name->data);
    }
    if (positional > cls->param_count) {
        if (core != NULL) {
            /*
             * An error counts its message as the first argument: E takes at most 2 arguments
             * (message, n), got 3
             */
            char list[256] = "message";
            size_t used = strlen(list);
            for (size_t i = 0; i < cls->param_count; i++) {
                const LzString *field = field_at(cls, cls->params[i])->name;
                if (used + field->length + 3 >= sizeof list) {
                    (void)snprintf(list + used, sizeof list - used, ", ...");
                    break;
                }
                used += (size_t)snprintf(list + used, sizeof list - used, ", %s", field->data);
            }
            char signature[256];
            signature_help(cls, true, signature, sizeof signature);
            return fail(vm, LZ_ERR_ARITY, signature, "%s takes at most %zu argument%s (%s), got %zu", cls->name->data,
                        cls->param_count + 1, cls->param_count + 1 == 1 ? "" : "s", list, positional + 1);
        }
        char signature[256];
        signature_help(cls, false, signature, sizeof signature);
        return fail(vm, LZ_ERR_ARITY, signature, "%s takes at most %zu argument%s, got %zu", cls->name->data,
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
        const LzMember *member = field_at(cls, cls->params[i]);
        if (given[i] == NULL && member->thunk.type == LZ_VAL_NIL) {
            char signature[256];
            signature_help(cls, false, signature, sizeof signature);
            return fail(vm, LZ_ERR_ARITY, signature, "%s needs '%s'", cls->name->data,
                        member->name->data);
        }
    }

    LzInstance *instance = instance_alloc(vm, cls);
    if (instance == NULL) {
        return lz_vm_out_of_memory(vm);
    }
    if (core != NULL) {
        core->payload = instance; /* the creation reference moves to the error */
        instance->owner_error = &core->base;
        instance->holders++; /* the error holds its payload, which the cycle check must see */
    }
    LzStatus status = LZ_OK;
    for (size_t i = 0; i < cls->param_count && status == LZ_OK; i++) {
        if (given[i] != NULL) {
            status = lz_instance_store(vm, instance, field_at(cls, cls->params[i]), given[i]);
        }
    }
    /*
     * The rest come from their defaults, with the parent's fields first and then the class's
     * own, in declaration order.
     */
    for (size_t slot = 0; slot < cls->field_count && status == LZ_OK; slot++) {
        const LzMember *member = field_at(cls, slot);
        if (((instance->unset >> slot) & 1u) == 0 || member->thunk.type == LZ_VAL_NIL) {
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
    if (core != NULL) {
        if (status != LZ_OK) {
            return status; /* the caller owns the error and drops it, and the payload with it */
        }
        *result = lz_value_object(&core->base);
        return LZ_OK;
    }
    if (status != LZ_OK) {
        lz_object_release(&vm->heap, &instance->base);
        return status;
    }
    *result = lz_value_object(&instance->base);
    return LZ_OK;
}

LzStatus lz_class_construct(LzVM *vm, LzClass *cls, size_t positional, const LzValue *argv,
                            size_t named_count, const char *names, size_t names_length,
                            const LzValue *named, LzValue *result) {
    return construct_impl(vm, cls, positional, argv, named_count, names, names_length, named, NULL, result);
}

/*
 * `Kind(message, field..., cause: e, data: v, field: v)` takes the message and then the fields,
 * in the order of the class. `cause` and `data` are only ever given by name.
 */
LzStatus lz_error_class_construct(LzVM *vm, LzClass *cls, uint8_t kind, size_t positional,
                                  const LzValue *argv, size_t named_count, const char *names,
                                  size_t names_length, const LzValue *named, LzValue *result) {
    if (positional < 1) {
        return lz_vm_set_error(vm, LZ_ERR_ARITY, "%s(): wrong number of arguments (%zu given)",
                               lz_vm_error_kind_name(vm, kind), positional);
    }
    /* Split the named arguments. cause and data are the error's, and the rest are fields. */
    LzValue cause = lz_value_nil();
    LzValue data = lz_value_nil();
    bool seen_cause = false;
    bool seen_data = false;
    LzValue rest[LZ_CLASS_FIELDS_MAX + 2];
    char rest_names[LZ_CLASS_FIELDS_MAX * 64 + 1];
    size_t rest_count = 0;
    size_t rest_length = 0;
    size_t position = 0;
    for (size_t k = 0; k < named_count; k++) {
        size_t end = position;
        while (end < names_length && names[end] != '\0') {
            end++;
        }
        size_t length = end - position;
        const char *text = names + position;
        position = end + 1;
        if (length == 5 && memcmp(text, "cause", 5) == 0) {
            if (seen_cause) {
                return lz_vm_set_error(vm, LZ_ERR_ARITY, "%s(): got multiple values for argument 'cause'",
                                       cls->name->data);
            }
            seen_cause = true;
            cause = named[k];
        } else if (length == 4 && memcmp(text, "data", 4) == 0) {
            if (seen_data) {
                return lz_vm_set_error(vm, LZ_ERR_ARITY, "%s(): got multiple values for argument 'data'",
                                       cls->name->data);
            }
            seen_data = true;
            data = named[k];
        } else {
            if (rest_count >= LZ_CLASS_FIELDS_MAX + 2 || rest_length + length + 1 > sizeof rest_names) {
                return lz_vm_set_error(vm, LZ_ERR_ARITY, "%s(): too many named arguments", cls->name->data);
            }
            memcpy(rest_names + rest_length, text, length);
            rest_names[rest_length + length] = '\0';
            rest_length += length + 1;
            rest[rest_count++] = named[k];
        }
    }
    LzValue core_value = lz_value_nil();
    LzStatus status = lz_error_construct_core(vm, kind, &argv[0], &cause, &data, &core_value);
    if (status != LZ_OK) {
        return status;
    }
    status = construct_impl(vm, cls, positional - 1, argv + 1, rest_count, rest_names, rest_length, rest,
                            (LzError *)core_value.as.object, result);
    if (status != LZ_OK) {
        lz_object_release(&vm->heap, core_value.as.object);
    }
    return status;
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
    /*
     * A link field gets a fresh cell pointing at the same target, so the two objects don't share
     * one.
     */
    for (size_t i = 0; i < source->cls->field_count; i++) {
        const LzMember *member = field_at(source->cls, (uint16_t)i);
        if (member->is_link && lz_value_is_link(&copy->fields[i])) {
            const LzLink *from = lz_value_as_link(&copy->fields[i]);
            LzLink *to = lz_link_new(&vm->heap, from->target);
            if (to == NULL) {
                lz_object_release(&vm->heap, &copy->base); /* releases its fields and frees it */
                return lz_vm_out_of_memory(vm);
            }
            if (from->target != NULL && !lz_link_track(vm, to)) {
                lz_object_release(&vm->heap, &to->base);
                lz_object_release(&vm->heap, &copy->base);
                return lz_vm_out_of_memory(vm);
            }
            LzValue fresh = lz_value_object(&to->base);
            lz_value_release(&vm->heap, &copy->fields[i]);
            copy->fields[i] = fresh;
        }
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
        status = lz_instance_store(vm, copy, field_at(cls, cls->params[param]), &named[k]);
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

/*
 * A new error with the message, kind, cause and data of `source` and a copy of its payload. It
 * has no trace and init doesn't run.
 */
static LzStatus error_clone(LzVM *vm, const LzError *source, LzError **out) {
    LzError *error = NULL;
    LzStatus status = lz_error_new(vm, source->kind, source->message->data, source->message->length, &error);
    if (status != LZ_OK) {
        return status;
    }
    lz_error_set_extra(vm, error, &source->cause, &source->data);
    if (source->payload != NULL) {
        const LzInstance *from = source->payload;
        LzInstance *copy = instance_alloc(vm, from->cls);
        if (copy == NULL) {
            lz_object_release(&vm->heap, &error->base);
            return lz_vm_out_of_memory(vm);
        }
        for (size_t i = 0; i < from->cls->field_count; i++) {
            copy->fields[i] = from->fields[i];
            lz_value_retain(&copy->fields[i]);
            lz_note_container_store(&vm->heap, &copy->fields[i]);
        }
        copy->unset = from->unset;
        error->payload = copy; /* the creation reference moves to the error */
        copy->owner_error = &error->base;
        copy->holders++;
    }
    *out = error;
    return LZ_OK;
}

LzStatus lz_error_copy(LzVM *vm, const LzError *source, LzValue *result) {
    LzError *error = NULL;
    LzStatus status = error_clone(vm, source, &error);
    if (status != LZ_OK) {
        return status;
    }
    *result = lz_value_object(&error->base);
    return LZ_OK;
}

LzStatus lz_error_with(LzVM *vm, const LzError *source, size_t named_count, const char *names,
                       size_t names_length, const LzValue *named, LzValue *result) {
    LzError *error = NULL;
    LzStatus status = error_clone(vm, source, &error);
    if (status != LZ_OK) {
        return status;
    }
    LzClass *cls = error->payload != NULL ? error->payload->cls : NULL;
    bool seen[LZ_CLASS_FIELDS_MAX] = {false};
    bool seen_message = false;
    bool seen_cause = false;
    bool seen_data = false;
    for (size_t k = 0; k < named_count && status == LZ_OK; k++) {
        LzString *name = NULL;
        if (name_at(vm, names, names_length, k, &name) == NULL) {
            status = lz_vm_out_of_memory(vm);
            break;
        }
        const LzValue *value = &named[k];
        if (strcmp(name->data, "message") == 0) {
            if (seen_message) {
                status = fail(vm, LZ_ERR_ARITY, NULL, "'message' was given twice");
            } else if (value->type != LZ_VAL_OBJECT || value->as.object->type != LZ_OBJ_STRING) {
                status = lz_vm_set_error(vm, LZ_ERR_TYPE, "the message of an error must be a string");
            } else {
                seen_message = true;
                LzString *old = error->message;
                error->message = (LzString *)value->as.object;
                lz_object_retain(&error->message->base);
                lz_object_release(&vm->heap, &old->base);
            }
        } else if (strcmp(name->data, "cause") == 0) {
            if (seen_cause) {
                status = fail(vm, LZ_ERR_ARITY, NULL, "'cause' was given twice");
            } else if (value->type != LZ_VAL_NIL && !lz_value_is_error(value)) {
                status = lz_vm_set_error(vm, LZ_ERR_TYPE, "the cause of an error must be an error or nil");
            } else {
                seen_cause = true;
                LzValue old = error->cause;
                lz_value_retain(value);
                error->cause = *value;
                lz_value_release(&vm->heap, &old);
            }
        } else if (strcmp(name->data, "data") == 0) {
            if (seen_data) {
                status = fail(vm, LZ_ERR_ARITY, NULL, "'data' was given twice");
            } else {
                seen_data = true;
                LzValue old = error->data;
                lz_value_retain(value);
                error->data = *value;
                lz_note_container_store(&vm->heap, &error->data);
                lz_value_release(&vm->heap, &old);
            }
        } else if (cls == NULL) {
            status = fail(vm, LZ_ERR_ARITY, NULL, "%s has no field '%s'", error->kind_name->data, name->data);
        } else {
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
            status = lz_instance_store(vm, error->payload, field_at(cls, cls->params[param]), value);
        }
    }
    if (status == LZ_OK && error->payload != NULL) {
        status = finish_instance(vm, error->payload);
    }
    if (status != LZ_OK) {
        lz_object_release(&vm->heap, &error->base);
        return status;
    }
    *result = lz_value_object(&error->base);
    return LZ_OK;
}

static LzStatus native_copy_fn(LzVM *vm, int argc, const LzValue *argv, LzValue *result) {
    (void)argc;
    if (lz_value_is_error(&argv[0])) {
        return lz_error_copy(vm, (const LzError *)argv[0].as.object, result);
    }
    if (!lz_value_is_instance(&argv[0])) {
        return lz_vm_set_error(vm, LZ_ERR_RUNTIME, "copy() needs an object");
    }
    return lz_instance_copy(vm, lz_value_as_instance(&argv[0]), result);
}

static LzStatus native_with_fn(LzVM *vm, int argc, const LzValue *argv, LzValue *result) {
    (void)argc;
    (void)result;
    if (lz_value_is_error(&argv[0])) {
        return fail(vm, LZ_ERR_ARITY, NULL, "with() takes named fields, like e.with(needed: 5)");
    }
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

/*
 * A link field stores a cell, not the object, and reading it gives the target, or nil once the
 * target is gone. Everywhere else in class.c the cell is the field's value, which is just an
 * object with no edges of its own.
 */
static LzStatus read_field(LzVM *vm, const LzInstance *instance, const LzMember *member, LzValue *out) {
    if (((instance->unset >> member->slot) & 1u) != 0) {
        return unset_error(vm, instance->cls, member);
    }
    *out = instance->fields[member->slot];
    lz_value_retain(out);
    if (member->is_link && lz_value_is_link(out)) {
        LzValue target = lz_link_deref(lz_value_as_link(out));
        lz_value_release(&vm->heap, out);
        *out = target;
    }
    return LZ_OK;
}

/*
 * The cell a link field should hold for `value`. That's the value itself when it's already a
 * link, link(nil) for nil, and a fresh cell otherwise. *made receives a reference the caller
 * owns and must release when a cell was made, else nil. Fails with the TypeError of link().
 */
static LzStatus link_value_for(LzVM *vm, const LzValue *value, LzValue *cell, LzValue *made) {
    *made = lz_value_nil();
    if (lz_value_is_link(value)) {
        *cell = *value;
        lz_value_retain(cell); /* the caller releases both `cell` (stored) and `made` */
        return LZ_OK;
    }
    LzObject *target = NULL;
    if (value->type == LZ_VAL_OBJECT && LZ_OBJ_LINK != value->as.object->type) {
        switch (value->as.object->type) {
        case LZ_OBJ_INSTANCE:
        case LZ_OBJ_ARRAY:
        case LZ_OBJ_DICT:
        case LZ_OBJ_CLOSURE:
        case LZ_OBJ_FUNCTION:
            target = value->as.object;
            break;
        default:
            break;
        }
    }
    if (target == NULL && value->type != LZ_VAL_NIL) {
        LzStatus status = lz_vm_set_error(
            vm, LZ_ERR_TYPE,
            "link() needs an object that can be shared (instance, array, dict or function), got %s",
            lz_value_kind_name(value));
        lz_vm_set_help(vm, "strings and numbers are copied, not shared; link an instance, array, dict or function");
        return status;
    }
    LzLink *link = lz_link_new(&vm->heap, target);
    if (link == NULL) {
        return lz_vm_out_of_memory(vm);
    }
    if (target != NULL && !lz_link_track(vm, link)) {
        lz_object_release(&vm->heap, &link->base);
        return lz_vm_out_of_memory(vm);
    }
    *made = lz_value_object(&link->base);
    *cell = *made;
    lz_value_retain(cell); /* the caller releases both, and `cell` stays in the field while `made` is dropped */
    return LZ_OK;
}

static LzStatus instance_get(LzVM *vm, uint64_t owner, LzInstance *instance,
                             const LzString *name, LzValue *result, bool for_write) {
    LzClass *cls = instance->cls;
    LzFound found;
    if (!lz_class_lookup(cls, name, &found)) {
        return no_member(vm, cls, owner, name, "field", true, false);
    }
    const LzMember *member = found.member;
    if (!member_visible(owner, found.declared_in, member)) {
        return private_error(vm, found.declared_in, member);
    }
    switch (member->kind) {
    case LZ_MEMBER_FIELD:
        if (for_write && member->vis == LZ_VIS_PUB_READ && !lz_class_sees(vm, owner, found.declared_in)) {
            return read_only_error(vm, found.declared_in, member);
        }
        return read_field(vm, instance, member, result);
    case LZ_MEMBER_GETTER: {
        LzValue self = self_of(instance);
        return call_closure(vm, &member->value, 1, &self, result);
    }
    case LZ_MEMBER_METHOD: {
        LzValue self = self_of(instance);
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

static LzStatus class_get(LzVM *vm, uint64_t owner, LzClass *cls, const LzString *name,
                          LzValue *result) {
    LzFound found;
    if (!lz_class_lookup(cls, name, &found)) {
        return no_member(vm, cls, owner, name, "constant or function", false, false);
    }
    const LzMember *member = found.member;
    if (!member_visible(owner, found.declared_in, member)) {
        return private_error(vm, found.declared_in, member);
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

LzStatus lz_vm_member_get(LzVM *vm, uint64_t owner, const LzValue *target,
                          const LzString *name, LzValue *result, bool for_write) {
    if (lz_value_is_instance(target)) {
        return instance_get(vm, owner, lz_value_as_instance(target), name, result, for_write);
    }
    return class_get(vm, owner, lz_value_as_class(target), name, result);
}

LzStatus lz_vm_ext_get(LzVM *vm, uint64_t owner, const LzValue *target, const LzString *name,
                      LzValue *result, bool *found) {
    *found = false;
    if (vm->ext_count == 0) {
        return LZ_OK;
    }
    LzFound hit;
    if (!lz_ext_find_value(vm, target, name, &hit)) {
        return LZ_OK;
    }
    *found = true;
    if (!member_visible(owner, hit.declared_in, hit.member)) {
        return private_error(vm, hit.declared_in, hit.member);
    }
    LzValue self = *target;
    if (hit.member->kind == LZ_MEMBER_GETTER) {
        return call_closure(vm, &hit.member->value, 1, &self, result);
    }
    LzBound *bound = lz_bound_new(&vm->heap, &hit.member->value, &self, false);
    if (bound == NULL) {
        return lz_vm_out_of_memory(vm);
    }
    *result = lz_value_object(&bound->base);
    return LZ_OK;
}

/*
 * A link stands in for its target when a method or an extension is looked up on it, so
 * `n.parent`, a link field, can call the methods of the object it points at.
 */
static bool as_link_target(const LzValue *value, LzValue *target) {
    if (!lz_value_is_link(value)) {
        return false;
    }
    *target = lz_link_deref(lz_value_as_link(value));
    return true;
}

LzStatus lz_vm_link_target(LzVM *vm, const LzValue *value, LzValue *target, bool *is_link) {
    (void)vm;
    *is_link = as_link_target(value, target);
    return LZ_OK;
}

static LzStatus dead_link_error(LzVM *vm, LzStatus status) {
    (void)lz_vm_set_error(vm, status, "this link points at nothing any more (its object was freed)");
    lz_vm_set_help(vm, "check l.alive before using it, or keep the owner of the object alive as long as you need it");
    return status;
}

LzStatus lz_vm_link_member_get(LzVM *vm, uint64_t owner, const LzValue *value, const LzString *name,
                               LzValue *result) {
    LzValue target = lz_link_deref(lz_value_as_link(value));
    if (target.type == LZ_VAL_NIL) {
        return dead_link_error(vm, LZ_ERR_TYPE);
    }
    if (!lz_value_is_instance(&target)) {
        /*
         * Only an instance has fields and methods to read through its link, and the rest needs
         * .target.
         */
        lz_value_release(&vm->heap, &target);
        return lz_vm_link_error(vm);
    }
    LzStatus status = lz_vm_member_get(vm, owner, &target, name, result, false);
    lz_value_release(&vm->heap, &target);
    return status;
}

LzStatus lz_vm_link_member_set(LzVM *vm, uint64_t owner, const LzValue *value, const LzString *name,
                               const LzValue *assigned) {
    LzValue target = lz_link_deref(lz_value_as_link(value));
    if (target.type == LZ_VAL_NIL) {
        return dead_link_error(vm, LZ_ERR_TYPE);
    }
    LzStatus status = lz_vm_member_set(vm, owner, &target, name, assigned);
    lz_value_release(&vm->heap, &target);
    return status;
}

LzStatus lz_vm_link_ext_get(LzVM *vm, uint64_t owner, const LzValue *value, const LzString *name,
                           LzValue *result, bool *found) {
    LzValue target = lz_link_deref(lz_value_as_link(value));
    LzStatus status = lz_vm_ext_get(vm, owner, &target, name, result, found);
    lz_value_release(&vm->heap, &target);
    return status;
}

LzStatus lz_vm_member_set(LzVM *vm, uint64_t owner, const LzValue *target,
                          const LzString *name, const LzValue *value) {
    if (lz_value_is_class(target)) {
        LzClass *cls = lz_value_as_class(target);
        LzFound at;
        const LzMember *member = lz_class_lookup(cls, name, &at) ? at.member : NULL;
        if (member != NULL && member->kind == LZ_MEMBER_CONST) {
            return fail(vm, LZ_ERR_RUNTIME, "a constant is fixed when the class is defined; use a field (pub x = 0) for a value that changes", "'%s' is a constant and cannot be assigned", name->data);
        }
        if (member == NULL) {
            return no_member(vm, cls, owner, name, "constant or function", false, true);
        }
        return fail(vm, LZ_ERR_RUNTIME, NULL, "'%s' of %s cannot be assigned", name->data, cls->name->data);
    }
    LzInstance *instance = lz_value_as_instance(target);
    LzClass *cls = instance->cls;
    LzFound found;
    if (!lz_class_lookup(cls, name, &found)) {
        return no_member(vm, cls, owner, name, "field", true, true);
    }
    const LzMember *member = found.member;
    if (!member_visible(owner, found.declared_in, member)) {
        return private_error(vm, found.declared_in, member);
    }
    switch (member->kind) {
    case LZ_MEMBER_FIELD:
        if (member->vis == LZ_VIS_PUB_READ && !lz_class_sees(vm, owner, found.declared_in)) {
            return read_only_error(vm, found.declared_in, member);
        }
        return lz_instance_store(vm, instance, member, value);
    case LZ_MEMBER_GETTER:
        return fail(vm, LZ_ERR_RUNTIME, "change the fields it is computed from, or write a method to set them",
                    "'%s' is a computed property and cannot be assigned", name->data);
    case LZ_MEMBER_CONST:
        return fail(vm, LZ_ERR_RUNTIME, "a constant is fixed when the class is defined; use a field (pub x = 0) for a value that changes", "'%s' is a constant and cannot be assigned", name->data);
    case LZ_MEMBER_METHOD:
    case LZ_MEMBER_STATIC:
        return fail(vm, LZ_ERR_RUNTIME, NULL, "'%s' is a function and cannot be assigned", name->data);
    }
    return LZ_OK;
}

/*
 * Replaces the fallback on top of the stack by `callee`, which is a new reference and is
 * consumed.
 */
/* "a string", "an int": the noun with its article, for a label. */
static const char *article_for(const char *noun) {
    return strchr("aeiou", noun[0]) != NULL ? "an" : "a";
}

LzStatus lz_vm_not_a_method_error(LzVM *vm, const char *name, const char *type) {
    LzStatus status =
        lz_vm_set_error(vm, LZ_ERR_RUNTIME, "'%s' is a function, not a method of %s", name, type);
    lz_vm_set_label(vm, "%s %s has no method '%s'", article_for(type), type, name);
    lz_vm_set_help(vm,
                   "call it as a function, %s(value), or make it a method of %s: extend %s { pub fn %s(self) { ... } }",
                   name, type, type, name);
    return status;
}

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

/* The natives behind x.with(...) and x.copy(), made when first needed. */
static LzStatus install_copy_with(LzVM *vm, const LzString *name, size_t fallback) {
    bool is_with = strcmp(name->data, "with") == 0;
    LzObject **slot = is_with ? &vm->native_with : &vm->native_copy;
    if (*slot == NULL) {
        *slot = is_with ? make_native(vm, "with", native_with_fn, 255) : make_native(vm, "copy", native_copy_fn, 1);
        if (*slot == NULL) {
            return lz_vm_out_of_memory(vm);
        }
    }
    lz_object_retain(*slot);
    return set_callee(vm, fallback, lz_value_object(*slot));
}

static LzStatus instance_callee(LzVM *vm, uint64_t owner, const LzString *name, int mode,
                                size_t receiver_index) {
    LzValue receiver = vm->stack[receiver_index];
    /*
     * A method called through a link runs on the target's class, but `self` stays the link in
     * the receiver slot, so a method that reads its own fields goes through the link like any
     * caller.
     */
    LzValue stand = receiver;
    if (lz_value_is_link(&stand)) {
        /*
         * No retain, because the lookup only borrows the target and it owns nothing when it
         * returns.
         */
        const LzLink *cell = (const LzLink *)stand.as.object;
        stand = cell->target != NULL ? lz_value_object(cell->target) : lz_value_nil();
    }
    LzInstance *instance = lz_value_is_instance(&stand) ? lz_value_as_instance(&stand) : payload_of(&stand);
    if (instance == NULL) {
        return dead_link_error(vm, LZ_ERR_TYPE);
    }
    if (lz_value_is_link(&receiver) && (strcmp(name->data, "copy") == 0 || strcmp(name->data, "with") == 0)) {
        return lz_vm_link_error(vm); /* copying a cell would not copy the object: read .target first */
    }
    if (lz_value_is_link(&receiver) && lz_value_is_instance(&stand)) {
        /* `self` is the object and never the cell, so the slot takes the retained target. */
        LzValue target = stand;
        lz_value_retain(&target);
        lz_value_release(&vm->heap, &vm->stack[receiver_index]);
        vm->stack[receiver_index] = target;
    }
    LzClass *cls = instance->cls;
    size_t fallback = receiver_index + 1;
    LzFound hit;
    const LzMember *member = lz_class_lookup(cls, name, &hit) ? hit.member : NULL;
    if (member != NULL && member->kind == LZ_MEMBER_METHOD && strcmp(name->data, "init") == 0) {
        return fail(vm, LZ_ERR_RUNTIME, NULL, "init() runs automatically when an object is created");
    }
    if (member != NULL) {
        if (!member_visible(owner, hit.declared_in, member)) {
            return private_error(vm, hit.declared_in, member);
        }
        switch (member->kind) {
        case LZ_MEMBER_METHOD:
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
    /*
     * Not a member, so with() and copy() belong to every instance, and then come the builtin
     * methods.
     */
    if (strcmp(name->data, "with") == 0 || strcmp(name->data, "copy") == 0) {
        return install_copy_with(vm, name, fallback);
    }
    if (mode == 0) {
        return LZ_OK; /* the fallback compiled with the call: a builtin method or a function of that name */
    }
    const LzValue *found = lz_table_get(&vm->methods, name);
    if (found == NULL) {
        const LzValue *global = lz_vm_lookup(vm, name);
        if (global != NULL && (lz_kind_of(global) & LZ_KIND_FUNCTION) != 0) {
            return lz_vm_not_a_method_error(vm, name->data, cls->name->data); /* no fallback to functions */
        }
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

static LzStatus class_callee(LzVM *vm, uint64_t owner, const LzString *name, size_t receiver_index) {
    LzValue receiver = vm->stack[receiver_index];
    if (lz_value_is_link(&receiver)) {
        /*
         * The class is the target. `self` for a static is the class itself, so the slot becomes
         * it.
         */
        LzValue target = lz_link_deref(lz_value_as_link(&receiver));
        if (target.type == LZ_VAL_NIL) {
            return dead_link_error(vm, LZ_ERR_TYPE);
        }
        lz_value_retain(&target);
        lz_value_release(&vm->heap, &vm->stack[receiver_index]);
        vm->stack[receiver_index] = target;
        receiver = target;
    }
    LzClass *cls = lz_value_as_class(&receiver);
    LzFound found;
    if (!lz_class_lookup(cls, name, &found)) {
        return no_member(vm, cls, owner, name, "function or constant", false, false);
    }
    const LzMember *member = found.member;
    if (!member_visible(owner, found.declared_in, member)) {
        return private_error(vm, found.declared_in, member);
    }
    switch (member->kind) {
    case LZ_MEMBER_STATIC:
    case LZ_MEMBER_METHOD:
    case LZ_MEMBER_CONST:
        /* The call passes the class as the first argument, and the bound callee drops it again. */
        return set_bound_callee(vm, receiver_index + 1, &member->value, NULL, true);
    case LZ_MEMBER_FIELD:
    case LZ_MEMBER_GETTER:
        return fail(vm, LZ_ERR_RUNTIME, NULL, "'%s' belongs to an object of %s, not to the class itself",
                    name->data, cls->name->data);
    }
    return LZ_OK;
}

static uint64_t calling_owner(const LzVM *vm) {
    if (vm->frame_count == 0) {
        return 0;
    }
    const LzCallFrame *frame = &vm->frames[vm->frame_count - 1];
    return frame->closure != NULL ? frame->closure->function->owner_id : 0;
}

static const char *value_class_name(const LzValue *value) {
    return lz_value_is_instance(value) ? lz_value_as_instance(value)->cls->name->data : lz_value_kind_name(value);
}

/*
 * Runs the operator `symbol` of `receiver`'s class with (receiver, other), or with the receiver
 * alone when `other` is NULL. *found is false when its class has none.
 */
static LzStatus run_operator(LzVM *vm, const LzValue *receiver, const char *symbol, const LzValue *other,
                             LzValue *result, bool *found) {
    *found = false;
    if (!lz_value_is_instance(receiver)) {
        return LZ_OK;
    }
    LzString *name = lz_vm_intern(vm, symbol, strlen(symbol));
    if (name == NULL) {
        return lz_vm_out_of_memory(vm);
    }
    LzFound hit;
    if (!lz_class_lookup(lz_value_as_instance(receiver)->cls, name, &hit) || hit.member->kind != LZ_MEMBER_METHOD) {
        return LZ_OK;
    }
    if (!member_visible(calling_owner(vm), hit.declared_in, hit.member)) {
        return private_error(vm, hit.declared_in, hit.member);
    }
    *found = true;
    LzValue argv[2] = {*receiver, other != NULL ? *other : lz_value_nil()};
    return call_closure(vm, &hit.member->value, other != NULL ? 2 : 1, argv, result);
}

/* The result of `operator ==` or `operator <` must be a bool. */
static LzStatus expect_bool(LzVM *vm, const LzValue *receiver, const char *symbol, LzValue *result) {
    if (result->type == LZ_VAL_BOOL) {
        return LZ_OK;
    }
    LzStatus status = lz_vm_set_error(vm, LZ_ERR_TYPE, "operator %s of %s must return a bool, got %s", symbol,
                                      value_class_name(receiver), lz_value_kind_name(result));
    lz_value_release(&vm->heap, result);
    return status;
}

static const char *binary_symbol(LzOpCode op) {
    switch (op) {
    case LZ_OP_ADD:
        return "+";
    case LZ_OP_SUBTRACT:
        return "-";
    case LZ_OP_MULTIPLY:
        return "*";
    case LZ_OP_DIVIDE:
        return "/";
    case LZ_OP_MODULO:
        return "%";
    default:
        return NULL;
    }
}

static const char *operator_text(LzOpCode op) {
    switch (op) {
    case LZ_OP_FLOOR_DIVIDE:
        return "//";
    case LZ_OP_BIT_AND:
        return "&";
    case LZ_OP_BIT_OR:
        return "|";
    case LZ_OP_BIT_XOR:
        return "^";
    case LZ_OP_SHL:
        return "<<";
    case LZ_OP_SHR:
        return ">>";
    default:
        return binary_symbol(op) != NULL ? binary_symbol(op) : "?";
    }
}

LzStatus lz_class_binary_operator(LzVM *vm, LzOpCode op, const LzValue *left, const LzValue *right,
                                  LzValue *result, bool *handled) {
    *handled = false;
    if (lz_value_is_instance(left)) {
        *handled = true;
        const char *symbol = binary_symbol(op);
        bool found = false;
        if (symbol != NULL) {
            LzStatus status = run_operator(vm, left, symbol, right, result, &found);
            if (status != LZ_OK || found) {
                return status;
            }
        }
        return lz_vm_set_error(vm, LZ_ERR_RUNTIME, "operator '%s' is not defined for %s", operator_text(op),
                               value_class_name(left));
    }
    const char *symbol = binary_symbol(op);
    if (symbol != NULL && lz_value_is_instance(right)) {
        LzString *name = lz_vm_intern(vm, symbol, strlen(symbol));
        LzFound hit;
        if (name != NULL && lz_class_lookup(lz_value_as_instance(right)->cls, name, &hit) &&
            hit.member->kind == LZ_MEMBER_METHOD) {
            *handled = true;
            return lz_vm_set_error(vm, LZ_ERR_RUNTIME,
                                   "%s defines '%s' only when it is on the left; write vec %s 2",
                                   value_class_name(right), symbol, symbol);
        }
    }
    return LZ_OK;
}

/* `a < b` uses the operator of a. */
static LzStatus less_than(LzVM *vm, const LzValue *a, const LzValue *b, bool *answer) {
    LzValue result = lz_value_nil();
    bool found = false;
    LzStatus status = run_operator(vm, a, "<", b, &result, &found);
    if (status != LZ_OK) {
        return status;
    }
    if (!found) {
        return lz_vm_set_error(vm, LZ_ERR_RUNTIME, "operator '<' is not defined for %s", value_class_name(a));
    }
    status = expect_bool(vm, a, "<", &result);
    if (status != LZ_OK) {
        return status;
    }
    *answer = result.as.boolean;
    return LZ_OK;
}

LzStatus lz_class_order_operator(LzVM *vm, LzOpCode op, const LzValue *left, const LzValue *right,
                                 LzValue *result, bool *handled) {
    *handled = false;
    if (!lz_value_is_instance(left) && !lz_value_is_instance(right)) {
        return LZ_OK;
    }
    *handled = true;
    bool answer = false;
    LzStatus status = LZ_OK;
    switch (op) {
    case LZ_OP_LESS:
        status = less_than(vm, left, right, &answer);
        break;
    case LZ_OP_GREATER:
        status = less_than(vm, right, left, &answer); /* a > b is b < a */
        break;
    case LZ_OP_LESS_EQUAL:
    case LZ_OP_GREATER_EQUAL: {
        bool forward = op == LZ_OP_LESS_EQUAL;
        status = forward ? less_than(vm, left, right, &answer) : less_than(vm, right, left, &answer);
        if (status == LZ_OK && !answer) {
            status = lz_vm_values_equal(vm, left, right, &answer); /* ... or equal */
        }
        break;
    }
    default:
        return lz_vm_set_error(vm, LZ_ERR_RUNTIME, "unknown comparison operator");
    }
    if (status != LZ_OK) {
        return status;
    }
    *result = lz_value_bool(answer);
    return LZ_OK;
}

LzStatus lz_class_negate_operator(LzVM *vm, const LzValue *operand, LzValue *result, bool *handled) {
    *handled = false;
    if (!lz_value_is_instance(operand)) {
        return LZ_OK;
    }
    *handled = true;
    bool found = false;
    LzStatus status = run_operator(vm, operand, "-@", NULL, result, &found);
    if (status != LZ_OK || found) {
        return status;
    }
    return lz_vm_set_error(vm, LZ_ERR_RUNTIME, "operator '-' is not defined for %s", value_class_name(operand));
}

/* Equality inside a comparison that has no VM argument, so the VM comparing is remembered here. */
static LzVM *equal_vm = NULL;
static LzStatus equal_pending = LZ_OK;

void lz_class_equal_scope_begin(LzVM *vm, LzVM **saved) {
    *saved = equal_vm;
    equal_vm = vm;
}

LzStatus lz_class_equal_scope_end(LzVM *saved) {
    equal_vm = saved;
    LzStatus status = equal_pending;
    equal_pending = LZ_OK;
    return status;
}

bool lz_class_equal_hook(const LzValue *left, const LzValue *right, bool *same) {
    if (equal_vm == NULL || !lz_value_is_instance(left)) {
        return false;
    }
    LzVM *vm = equal_vm;
    if (equal_pending != LZ_OK) {
        *same = false; /* an operator already failed, so stop quickly because the error is reported */
        return true;
    }
    LzValue result = lz_value_nil();
    bool found = false;
    LzStatus status = run_operator(vm, left, "==", right, &result, &found);
    if (status == LZ_OK && found) {
        status = expect_bool(vm, left, "==", &result);
    }
    if (status != LZ_OK) {
        equal_pending = status;
        *same = false;
        return true;
    }
    if (!found) {
        return false;
    }
    *same = result.as.boolean;
    return true;
}

LzStatus lz_vm_super_lookup(LzVM *vm, uint64_t owner, const LzString *name, int mode, size_t receiver) {
    const LzClass *code = lz_class_by_id(vm, owner);
    LzFound hit;
    if (code == NULL || code->parent == NULL || !lz_class_lookup(code->parent, name, &hit)) {
        return fail(vm, LZ_ERR_RUNTIME, NULL, "the parent of %s has no method '%s'",
                    code != NULL ? code->name->data : "this class", name->data);
    }
    const LzMember *member = hit.member;
    switch (member->kind) {
    case LZ_MEMBER_METHOD:
        if (mode == 1) {
            return fail(vm, LZ_ERR_RUNTIME, NULL, "'%s' is a function; write super.%s() with parentheses",
                        name->data, name->data);
        }
        lz_value_retain(&member->value);
        lz_value_release(&vm->heap, &vm->stack[receiver + 1]);
        vm->stack[receiver + 1] = member->value;
        return LZ_OK;
    case LZ_MEMBER_GETTER: {
        if (mode == 0) {
            return fail(vm, LZ_ERR_RUNTIME, NULL, "'%s' is a property; write super.%s without parentheses",
                        name->data, name->data);
        }
        LzValue self = vm->stack[receiver];
        LzValue value = lz_value_nil();
        LzStatus status = call_closure(vm, &member->value, 1, &self, &value);
        if (status != LZ_OK) {
            return status;
        }
        lz_value_release(&vm->heap, &vm->stack[receiver]);
        vm->stack[receiver] = value;
        return LZ_OK;
    }
    case LZ_MEMBER_STATIC:
        return fail(vm, LZ_ERR_RUNTIME, NULL, "'%s' is a static function; call it as %s.%s()", name->data,
                    hit.declared_in->name->data, name->data);
    case LZ_MEMBER_FIELD:
    case LZ_MEMBER_CONST:
        return fail(vm, LZ_ERR_RUNTIME, NULL, "'super' reaches functions and properties, and '%s' is %s of %s",
                    name->data, kind_phrase(member), hit.declared_in->name->data);
    }
    return LZ_OK;
}

LzStatus lz_vm_method_lookup(LzVM *vm, uint64_t owner, const LzString *name, int mode,
                             size_t receiver_index) {
    const LzValue *receiver = &vm->stack[receiver_index];
    if (lz_value_is_link(receiver)) {
        /*
         * A method call goes to the target's class, and the receiver slot keeps the link, which
         * a field of one of its methods may still read. A dead link has no methods.
         */
        LzValue target = lz_link_deref(lz_value_as_link(receiver));
        if (target.type != LZ_VAL_OBJECT) {
            if (mode != 0) {
                return dead_link_error(vm, LZ_ERR_RUNTIME);
            }
            return LZ_OK;
        }
        bool is_instance = lz_value_is_instance(&target);
        bool is_class = lz_value_is_class(&target);
        lz_value_release(&vm->heap, &target);
        if (is_instance) {
            return instance_callee(vm, owner, name, mode, receiver_index);
        }
        if (is_class) {
            return class_callee(vm, owner, name, receiver_index);
        }
    }
    if (lz_value_is_instance(receiver)) {
        return instance_callee(vm, owner, name, mode, receiver_index);
    }
    if (lz_value_is_class(receiver)) {
        return class_callee(vm, owner, name, receiver_index);
    }
    if (lz_value_is_error(receiver) && payload_of(receiver) != NULL) {
        LzFound member;
        if (lz_class_lookup(payload_of(receiver)->cls, name, &member)) {
            return instance_callee(vm, owner, name, mode, receiver_index);
        }
    }
    if (vm->ext_count != 0) {
        LzFound hit;
        if (lz_ext_find_value(vm, receiver, name, &hit)) {
            if (!member_visible(owner, hit.declared_in, hit.member)) {
                return private_error(vm, hit.declared_in, hit.member);
            }
            if (hit.member->kind == LZ_MEMBER_GETTER) {
                return fail(vm, LZ_ERR_RUNTIME, NULL, "'%s' is a property; write x.%s without parentheses",
                            name->data, name->data);
            }
            LzValue callee = hit.member->value;
            lz_value_retain(&callee);
            return set_callee(vm, receiver_index + 1, callee);
        }
    }
    if (lz_value_is_error(receiver) && (strcmp(name->data, "with") == 0 || strcmp(name->data, "copy") == 0)) {
        return install_copy_with(vm, name, receiver_index + 1); /* every error has them */
    }
    if (mode == 0) {
        return LZ_OK;
    }
    const LzValue *found = lz_table_get(&vm->methods, name);
    if (found == NULL) {
        const LzValue *global = lz_vm_lookup(vm, name);
        if (global != NULL && (lz_kind_of(global) & LZ_KIND_FUNCTION) != 0) {
            return lz_vm_not_a_method_error(vm, name->data, lz_kind_name(lz_kind_of(receiver)));
        }
        return lz_vm_set_error(vm, LZ_ERR_RUNTIME, "undefined variable '%s'", name->data);
    }
    LzValue callee = *found;
    lz_value_retain(&callee);
    return set_callee(vm, receiver_index + 1, callee);
}

#define WRITE_DEPTH_MAX 2000
static unsigned write_depth = 0;

static const LzMember *user_text_method(const LzClass *cls, const char *name) {
    for (; cls != NULL; cls = cls->parent) {
        for (size_t i = 0; i < cls->member_count; i++) {
            const LzMember *member = &cls->members[i];
            if (member->kind == LZ_MEMBER_METHOD && member->vis != LZ_VIS_PRIVATE &&
                strcmp(member->name->data, name) == 0) {
                return member;
            }
        }
    }
    return NULL;
}

static LzInstance *payload_of(const LzValue *value) {
    if (!lz_value_is_error(value)) {
        return NULL;
    }
    return ((const LzError *)value->as.object)->payload;
}

LzStatus lz_vm_error_member_get(LzVM *vm, uint64_t owner, const LzValue *error, const LzString *name,
                                LzValue *result, bool *found) {
    *found = false;
    LzInstance *payload = payload_of(error);
    if (payload == NULL) {
        return LZ_OK;
    }
    LzFound hit;
    if (!lz_class_lookup(payload->cls, name, &hit)) {
        return LZ_OK; /* message, kind, cause, data, trace and the builtin methods are the caller's */
    }
    *found = true;
    return instance_get(vm, owner, payload, name, result, false);
}

LzStatus lz_vm_error_member_set(LzVM *vm, uint64_t owner, const LzValue *error, const LzString *name,
                                const LzValue *value, bool *found) {
    *found = false;
    LzInstance *payload = payload_of(error);
    if (payload == NULL) {
        return LZ_OK;
    }
    LzFound hit;
    if (!lz_class_lookup(payload->cls, name, &hit)) {
        return LZ_OK;
    }
    *found = true;
    LzValue target = lz_value_object(&payload->base);
    return lz_vm_member_set(vm, owner, &target, name, value);
}

bool lz_error_is_class(const LzError *error, uint64_t id) {
    if (error->payload == NULL) {
        return false;
    }
    return lz_class_is_a(error->payload->cls, id);
}

static bool builtin_error_field(const char *name) {
    return strcmp(name, "message") == 0 || strcmp(name, "kind") == 0 || strcmp(name, "cause") == 0 ||
           strcmp(name, "data") == 0 || strcmp(name, "trace") == 0;
}

LzStatus lz_error_class_bind(LzVM *vm, LzClass *cls, uint8_t kind, uint8_t parent_kind) {
    for (size_t i = 0; i < cls->member_count; i++) {
        const LzMember *member = &cls->members[i];
        if (member->kind == LZ_MEMBER_FIELD && builtin_error_field(member->name->data)) {
            return link_fail(vm, cls->offset, NULL, "%s redeclares field '%s' of Error", cls->name->data,
                             member->name->data);
        }
    }
    LzClass *parent = lz_vm_error_class(vm, parent_kind);
    if (parent != NULL) {
        LzStatus status = lz_class_link(vm, cls, parent);
        if (status != LZ_OK) {
            return status;
        }
    }
    if (cls->trait_count > 0) {
        /* traits in the head, whose contracts and conflicts are checked as for a class */
        LzStatus checked = lz_class_finish(vm, cls);
        if (checked != LZ_OK) {
            return checked;
        }
    }
    LzStatus bound = lz_vm_error_set_class(vm, kind, cls);
    if (bound != LZ_OK) {
        return bound;
    }
    return lz_class_run_consts(vm, cls);
}

bool lz_instance_write(const LzInstance *instance, const LzWriter *writer, bool debug) {
    LzClass *cls = instance->cls;
    LzVM *vm = cls->vm;
    if (write_depth >= WRITE_DEPTH_MAX) {
        vm->text_status = lz_vm_set_error(vm, LZ_ERR_STACK_OVERFLOW, "stack overflow: value is nested too deeply to print");
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
    for (size_t slot = 0; ok && slot < cls->field_count; slot++) {
        const LzMember *member = field_at(cls, slot);
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

bool lz_error_write_text(const LzError *error, const LzWriter *writer, bool debug) {
    const LzInstance *payload = error->payload;
    if (payload == NULL) {
        return lz_error_write(error, writer);
    }
    LzClass *cls = payload->cls;
    LzVM *vm = cls->vm;
    const LzMember *user = user_text_method(cls, debug ? "to_debug" : "to_string");
    if (user != NULL && vm->frame_count > 0) {
        LzValue self = lz_value_object((LzObject *)&error->base);
        LzValue text = lz_value_nil();
        LzStatus status = call_closure(vm, &user->value, 1, &self, &text);
        bool ok;
        if (status != LZ_OK) {
            vm->text_status = status;
            return false;
        }
        if (text.type != LZ_VAL_OBJECT || text.as.object->type != LZ_OBJ_STRING) {
            vm->text_status = lz_vm_set_error(vm, LZ_ERR_TYPE, "%s() of %s must return a string, got %s",
                                              debug ? "to_debug" : "to_string", cls->name->data,
                                              lz_value_kind_name(&text));
            lz_value_release(&vm->heap, &text);
            return false;
        }
        ok = lz_writer_write(writer, ((const LzString *)text.as.object)->data,
                             ((const LzString *)text.as.object)->length);
        lz_value_release(&vm->heap, &text);
        return ok;
    }
    if (!debug) {
        return lz_error_write(error, writer);
    }
    bool ok = lz_writer_write(writer, error->kind_name->data, error->kind_name->length) &&
              lz_writer_write_cstr(writer, "(") && lz_value_write_debug(&(LzValue){LZ_VAL_OBJECT, {.object = &error->message->base}}, writer);
    for (size_t slot = 0; ok && slot < cls->field_count; slot++) {
        const LzMember *member = field_at(cls, slot);
        ok = lz_writer_write_cstr(writer, ", ") && lz_writer_write(writer, member->name->data, member->name->length) &&
             lz_writer_write_cstr(writer, ": ");
        if (!ok) {
            break;
        }
        ok = ((payload->unset >> member->slot) & 1u) != 0 ? lz_writer_write_cstr(writer, "<unset>")
                                                          : lz_value_write_debug(&payload->fields[member->slot], writer);
    }
    return ok && lz_writer_write_cstr(writer, ")");
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
