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
    uint8_t is_override; /* written with `override` */
    uint8_t is_required; /* in a trait: written without a body, so a class must provide it */
    uint16_t slot;    /* a field: its index in the instance */
    LzValue value;    /* method, static, getter: the closure. const: the value once initialised */
    LzValue thunk;    /* a field default or a const initialiser: a closure taking no argument, or nil */
    LzType type;      /* a field: the declared type (any when none) */
    uint32_t offset;  /* source offset of the declaration, for diagnostics */
} LzMember;

struct LzClass;
/* Where a field of an instance is declared: the class and the member index in that class. */
typedef struct LzFieldRef {
    struct LzClass *cls;
    uint16_t index;
} LzFieldRef;

typedef struct LzClass {
    LzObject base;
    uint64_t id;              /* unique in the VM for ever: types name a class by this, not by name */
    LzString *name;           /* interned, retained */
    struct LzClass *parent;   /* the class this one derives from (retained), or NULL; set by lz_class_link */
    LzVM *vm;                 /* the VM it belongs to (weak): instances write themselves through it */
    LzMember *members;
    size_t member_count;
    size_t member_capacity;
    size_t field_count;       /* slots in an instance: the parent's fields, then this class's own */
    size_t own_field_count;   /* the fields declared in this class's body */
    size_t param_count;       /* the fields that are arguments of the constructor, in order */
    uint16_t params[LZ_CLASS_FIELDS_MAX]; /* their slots: the parent's first, then this class's own */
    LzFieldRef field_refs[LZ_CLASS_FIELDS_MAX]; /* by slot */
    bool is_trait;            /* a trait: not created, its members are requirements and defaults */
    struct LzClass **traits;  /* the traits named in the head (retained): of a trait, the traits it extends */
    size_t trait_count;
    bool linked;              /* lz_class_link has run (a class without a parent links when sealed) */
    int init_member;          /* the member `init`, or -1 */
    bool consts_ready;        /* the initialisers of the constants have run */
    uint32_t offset;
    /* An `extend` block is a hidden class: it holds the members the block adds (is_ext) and is the
     * "class" its code is written in, so its private members are visible only to that block. */
    bool is_ext;
    uint16_t ext_kinds;       /* is_ext: the builtin kinds it extends (an LZ_KIND_* mask), or 0 for a class */
    uint64_t ext_target_id;   /* is_ext: the id of the class it extends, or 0 */
    uint16_t ext_error_kind;  /* is_ext: the error kind it extends + 1 (an error and its descendants), or 0 */
    LzString *ext_target;     /* is_ext: the name of the target as written (interned, retained) */
} LzClass;

typedef struct LzInstance {
    LzObject base;
    LzClass *cls;       /* retained */
    size_t holders;     /* containers that hold this instance, see lz_containers_would_cycle */
    uint32_t mark;      /* graph-walk stamp */
    uint64_t unset;     /* bit i: field i has no value yet */
    struct LzObject *owner_error; /* the payload of an error (not retained): the error is `self` for the code of the class */
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

/*
 * Is code written in the class with id `owner` (0: no class) inside `declaring`, the class that
 * declares a member? It is when `owner` is that class or derives from it: code of a class sees
 * the private members of itself and of its ancestors, never of its descendants.
 */
bool lz_class_sees(const LzVM *vm, uint64_t owner, const LzClass *declaring);
/* The live class with this id, or NULL. */
const LzClass *lz_class_by_id(const LzVM *vm, uint64_t id);
/* Is `cls` the class with this id, or derived from it? */
bool lz_class_is_a(const LzClass *cls, uint64_t id);

/* A member found along the chain from a class up through its parents. */
typedef struct LzFound {
    const LzMember *member;
    const LzClass *declared_in;
} LzFound;
bool lz_class_lookup(const LzClass *cls, const LzString *name, LzFound *out);

/*
 * Makes `parent` the parent of `cls` (when the `class` statement runs): lays the fields out after
 * the parent's and checks the pair. Returns LZ_OK, or an error in vm->error.
 */
LzStatus lz_class_link(LzVM *vm, LzClass *cls, LzClass *parent);
/* Adds `trait` to the traits of `cls` (the head `class A : B, T`); an error in vm->error if it is a repeat. */
LzStatus lz_class_add_trait(LzVM *vm, LzClass *cls, LzClass *trait);
/*
 * Checks a class once its head is linked (when the `class` statement runs): `override` against the
 * parent and the defaults of traits, and for a class that uses traits, that it provides what they
 * require and that no two traits give it the same name.
 */
LzStatus lz_class_finish(LzVM *vm, LzClass *cls);

/* The name to show for a class or instance kind. */
const char *lz_class_display_name(const LzClass *cls);

/* The constructor: positional values, then named ones (names separated by NUL bytes). */
LzStatus lz_class_construct(LzVM *vm, LzClass *cls, size_t positional, const LzValue *argv,
                            size_t named_count, const char *names, size_t names_length,
                            const LzValue *named, LzValue *result);

/*
 * `extend` (see docs/CLASSES.md). lz_vm_extend_register adds the block `ext` to the VM once its
 * statement runs: it resolves the target (`target`: the class value, or nil for a builtin kind),
 * refuses names the target already has, and invalidates the method cache. lz_ext_find looks a name
 * up for a class (and its ancestors) or for a builtin kind.
 */
LzStatus lz_vm_extend_register(LzVM *vm, LzClass *ext, const LzValue *target);
void lz_vm_release_extensions(LzVM *vm);
bool lz_ext_find_value(const LzVM *vm, const LzValue *value, const LzString *name, LzFound *out);
bool lz_ext_find_kind(const LzVM *vm, uint16_t kind, const LzString *name, LzFound *out);
bool lz_ext_find_class(const LzVM *vm, const LzClass *cls, const LzString *name, LzFound *out);

/* The initialisers of the constants (once). Runs script code: needs a running VM. */
LzStatus lz_class_run_consts(LzVM *vm, LzClass *cls);

/* x.name: the instance's field, property or bound method, or a class's constant or function. */
LzStatus lz_vm_member_get(LzVM *vm, uint64_t owner, const LzValue *target,
                          const LzString *name, LzValue *result, bool for_write);
/*
 * Error classes (`error Name : Parent { ... }`). The class is bound to its kind when the statement
 * runs. An error of such a kind is an ordinary error value that carries a payload: an instance of
 * the class that holds the fields. lz_error_class_bind checks and links `cls`.
 */
LzStatus lz_error_class_bind(LzVM *vm, LzClass *cls, uint8_t kind, uint8_t parent_kind);
LzStatus lz_error_class_construct(LzVM *vm, LzClass *cls, uint8_t kind, size_t positional,
                                  const LzValue *argv, size_t named_count, const char *names,
                                  size_t names_length, const LzValue *named, LzValue *result);
/* x.name / x.name = v / a method call on an error that carries a payload (*found: it is a member). */
LzStatus lz_vm_error_member_get(LzVM *vm, uint64_t owner, const LzValue *error, const LzString *name,
                                LzValue *result, bool *found);
LzStatus lz_vm_error_member_set(LzVM *vm, uint64_t owner, const LzValue *error, const LzString *name,
                                const LzValue *value, bool *found);
/* Is the error an instance of the class `id` or of a descendant? (False without a payload.) */
bool lz_error_is_class(const struct LzError *error, uint64_t id);

/* x.name for a value that is not an object: what `extend` added for its kind (*found says whether). */
LzStatus lz_vm_ext_get(LzVM *vm, uint64_t owner, const LzValue *target, const LzString *name,
                       LzValue *result, bool *found);
/* x.name = value. Takes no ownership of value. */
LzStatus lz_vm_member_set(LzVM *vm, uint64_t owner, const LzValue *target,
                          const LzString *name, const LzValue *value);
/*
 * OP_METHOD_LOOKUP on [receiver][fallback]: replaces the fallback by the callee to use. An instance
 * or class answers with its own member; anything else keeps the fallback (mode 0) or finds it
 * as a global by name (mode 1, where no fallback could be named when the call was compiled).
 */
/*
 * `super.name` (OP_SUPER): the stack holds [self] (mode 1) or [self][nil] (mode 0) at `receiver`.
 * The search starts at the parent of the class with id `owner`.
 */
LzStatus lz_vm_super_lookup(LzVM *vm, uint64_t owner, const LzString *name, int mode, size_t receiver);
/*
 * Operators (`operator +(self, other)`): an instance's class defines them under the names "+", "-",
 * "*", "/", "%", "==", "<" and, for the unary minus, "-@". These are the slow paths of arithmetic,
 * ordering and equality, reached when an operand is an instance; they return LZ_OK with
 * *handled = false when neither operand is one (the caller goes on as before).
 */
LzStatus lz_class_binary_operator(LzVM *vm, LzOpCode op, const LzValue *left, const LzValue *right,
                                  LzValue *result, bool *handled);
LzStatus lz_class_order_operator(LzVM *vm, LzOpCode op, const LzValue *left, const LzValue *right,
                                 LzValue *result, bool *handled);
LzStatus lz_class_negate_operator(LzVM *vm, const LzValue *operand, LzValue *result, bool *handled);
/*
 * Equality of two values compared without a VM in hand (nested arrays, dicts, fields): when `left`
 * is an instance whose class defines `operator ==`, the VM that is comparing (see
 * lz_class_equal_scope) runs it and *same is its answer. Returns false when there is nothing to run.
 * An error inside the operator is kept and handed back by lz_class_equal_scope_end.
 */
bool lz_class_equal_hook(const LzValue *left, const LzValue *right, bool *same);
void lz_class_equal_scope_begin(LzVM *vm, LzVM **saved);
LzStatus lz_class_equal_scope_end(LzVM *saved);
LzStatus lz_vm_method_lookup(LzVM *vm, uint64_t owner, const LzString *name, int mode,
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
/* copy() of an error: the message, kind, cause, data and fields; no trace, and init does not run. */
LzStatus lz_error_copy(LzVM *vm, const struct LzError *source, LzValue *result);
/* with() of an error: a copy with the named message, cause, data and fields replaced; init runs. */
LzStatus lz_error_with(LzVM *vm, const struct LzError *source, size_t named_count, const char *names,
                       size_t names_length, const LzValue *named, LzValue *result);

LzStatus lz_instance_with(LzVM *vm, const LzInstance *source, size_t named_count, const char *names,
                          size_t names_length, const LzValue *named, LzValue *result);

/* Releases what a dying object owns (called by lz_object_finalize). */
void lz_class_finalize(LzHeap *heap, LzClass *cls);
void lz_instance_finalize(LzHeap *heap, LzInstance *instance);
void lz_bound_finalize(LzHeap *heap, LzBound *bound);

/* At shutdown: drops the constants every class holds, which may point back at the class. */
void lz_vm_release_class_constants(LzVM *vm);

#endif
