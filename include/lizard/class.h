#ifndef LIZARD_CLASS_H
#define LIZARD_CLASS_H

#include "lizard/function.h"
#include "lizard/type.h"
#include "lizard/value.h"
#include "lizard/vm.h"

/*
 * Classes (stage 1: no inheritance, but see `parent`).
 *
 *   LzClass     what `class Name { ... }` makes. The compiler builds it while it reads the
 *               body (the methods are closures that capture nothing, because a class lives at
 *               the top level), so it is a constant of the chunk; running the `class`
 *               statement binds the name and then runs the initialisers of the constants.
 *   LzInstance  an object made by calling the class. Its fields live in `fields`, one slot per
 *               declared field, in declaration order.
 *   LzBound     `p.sum` without parentheses: a method tied to its receiver. It is also the
 *               shape of a call of a function held in a field (`p.callback(1)`), where the
 *               receiver the call passes is dropped again (`skip_first`).
 *
 * "Inside the class" is lexical: a function compiled in the body of class C carries C's name
 * as its `owner` (and so does every closure inside it). The visibility rules compare that name
 * with the class of the member; lz_class_sees is the one place that does the comparison, so a
 * later stage can widen it to "the class or a class derived from it".
 */

typedef enum LzMemberKind {
    LZ_MEMBER_FIELD,
    LZ_MEMBER_METHOD, /* takes `self` first */
    LZ_MEMBER_STATIC, /* a function without `self`, called as Class.name() */
    LZ_MEMBER_GETTER, /* `get name(self)` */
    LZ_MEMBER_CONST   /* `const NAME = expr`, owned by the class */
} LzMemberKind;

typedef enum LzVisibility {
    LZ_VIS_PRIVATE,
    LZ_VIS_PUB,
    LZ_VIS_PUB_READ /* readable from anywhere, writable only inside the class */
} LzVisibility;

#define LZ_CLASS_FIELDS_MAX 64
#define LZ_CLASS_MEMBERS_MAX 256

typedef struct LzMember {
    LzString *name;   /* interned, retained */
    uint8_t kind;     /* LzMemberKind */
    uint8_t vis;      /* LzVisibility */
    uint16_t slot;    /* a field: its index in the instance */
    LzValue value;    /* method, static, getter: the closure. const: the value once initialised */
    LzValue thunk;    /* a field default or a const initialiser: a closure taking no argument, or nil */
    LzType type;      /* a field: the declared type (any when none) */
    uint32_t offset;  /* source offset of the declaration, for diagnostics */
} LzMember;

typedef struct LzClass {
    LzObject base;
    LzString *name;           /* interned, retained */
    struct LzClass *parent;   /* stage 2: the class this one derives from (always NULL now) */
    LzVM *vm;                 /* the VM it belongs to (weak): instances write themselves through it */
    LzMember *members;
    size_t member_count;
    size_t member_capacity;
    size_t field_count;
    size_t param_count;       /* the fields that are arguments of the constructor, in order */
    uint16_t params[LZ_CLASS_FIELDS_MAX]; /* member indexes */
    int init_member;          /* the member `init`, or -1 */
    bool consts_ready;        /* the initialisers of the constants have run */
    uint32_t offset;
} LzClass;

typedef struct LzInstance {
    LzObject base;
    LzClass *cls;       /* retained */
    size_t holders;     /* containers that hold this instance, see lz_containers_would_cycle */
    uint32_t mark;      /* graph-walk stamp */
    uint64_t unset;     /* bit i: field i has no value yet */
    LzValue fields[];
} LzInstance;

typedef struct LzBound {
    LzObject base;
    LzValue callee;     /* a closure or any callable value */
    LzValue receiver;   /* the instance (nil when skip_first) */
    bool skip_first;
} LzBound;

static inline bool lz_value_is_class(const LzValue *value) {
    return value->type == LZ_VAL_OBJECT && value->as.object->type == LZ_OBJ_CLASS;
}
static inline bool lz_value_is_instance(const LzValue *value) {
    return value->type == LZ_VAL_OBJECT && value->as.object->type == LZ_OBJ_INSTANCE;
}
static inline bool lz_value_is_bound(const LzValue *value) {
    return value->type == LZ_VAL_OBJECT && value->as.object->type == LZ_OBJ_BOUND;
}
static inline LzClass *lz_value_as_class(const LzValue *value) {
    return (LzClass *)value->as.object;
}
static inline LzInstance *lz_value_as_instance(const LzValue *value) {
    return (LzInstance *)value->as.object;
}

/* Building a class (the compiler). NULL when out of memory. */
LzClass *lz_class_new(LzVM *vm, LzString *name, uint32_t offset);
/* Adds a member; false when out of memory. For a field the slot is assigned here. */
bool lz_class_add_member(LzClass *cls, LzString *name, LzMemberKind kind, LzVisibility vis,
                         uint32_t offset, LzMember **out);
/* Called once the body is read: works out the constructor parameters and `init`. */
void lz_class_seal(LzClass *cls);
const LzMember *lz_class_find(const LzClass *cls, const LzString *name);

/* Is code that belongs to the class named `owner` (NULL: no class) inside `cls`? */
bool lz_class_sees(const LzString *owner, const LzClass *cls);

/* The name to show for a class or instance kind. */
const char *lz_class_display_name(const LzClass *cls);

/* The constructor: positional values, then named ones (names separated by NUL bytes). */
LzStatus lz_class_construct(LzVM *vm, LzClass *cls, size_t positional, const LzValue *argv,
                            size_t named_count, const char *names, size_t names_length,
                            const LzValue *named, LzValue *result);

/* The initialisers of the constants (once). Runs script code: needs a running VM. */
LzStatus lz_class_run_consts(LzVM *vm, LzClass *cls);

/* x.name: the instance's field, property or bound method, or a class's constant or function. */
LzStatus lz_vm_member_get(LzVM *vm, const LzString *owner, const LzValue *target,
                          const LzString *name, LzValue *result, bool for_write);
/* x.name = value. Takes no ownership of value. */
LzStatus lz_vm_member_set(LzVM *vm, const LzString *owner, const LzValue *target,
                          const LzString *name, const LzValue *value);
/*
 * OP_METHOD_LOOKUP on [receiver][fallback]: replaces the fallback by the callee to use. An instance
 * or class answers with its own member; anything else keeps the fallback (mode 0) or finds it
 * as a global by name (mode 1, where no fallback could be named when the call was compiled).
 */
LzStatus lz_vm_method_lookup(LzVM *vm, const LzString *owner, const LzString *name, int mode,
                             size_t receiver_index);

/* A new bound method (a new reference). skip_first: drop the first argument of the call. */
LzBound *lz_bound_new(LzHeap *heap, const LzValue *callee, const LzValue *receiver, bool skip_first);

/* Stores into a field with the type, cycle and unset bookkeeping. */
LzStatus lz_instance_store(LzVM *vm, LzInstance *instance, const LzMember *member,
                           const LzValue *value);

/* Text: the built-in form, or the class's own to_string / to_debug. false: see vm->text_status. */
bool lz_instance_write(const LzInstance *instance, const LzWriter *writer, bool debug);

/* a == b for two instances (same class, all fields equal). Iterative; uses the budget like arrays. */
bool lz_instance_same_class(const LzInstance *a, const LzInstance *b);

/* identical(a, b) */
bool lz_values_identical(const LzValue *a, const LzValue *b);

/* Methods every instance has: with(...) and copy(). */
LzStatus lz_instance_copy(LzVM *vm, const LzInstance *source, LzValue *result);
LzStatus lz_instance_with(LzVM *vm, const LzInstance *source, size_t named_count, const char *names,
                          size_t names_length, const LzValue *named, LzValue *result);

/* Releases what a dying object owns (called by lz_object_finalize). */
void lz_class_finalize(LzHeap *heap, LzClass *cls);
void lz_instance_finalize(LzHeap *heap, LzInstance *instance);
void lz_bound_finalize(LzHeap *heap, LzBound *bound);

/* At shutdown: drops the constants every class holds, which may point back at the class. */
void lz_vm_release_class_constants(LzVM *vm);

#endif
