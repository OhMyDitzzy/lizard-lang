#ifndef LIZARD_INTERFACE_H
#define LIZARD_INTERFACE_H

#include <stdbool.h>
#include <stddef.h>

/*
 * The interface of a compiled unit, meaning what its `pub` declarations offer to other code.
 *
 * Without modules `pub` changes nothing at run time, but the compiler writes down every top-level
 * `pub` declaration, in source order, with its signature. A tool that links against the Lizard
 * library, like a stub generator, reads that record through the functions below. Everything here
 * is read-only and every pointer stays valid until the unit's chunk is released. The structures
 * themselves are opaque, so nothing of the compiler is exposed.
 *
 * Types are written as Lizard writes them, like `int`, `string?`, `int[]`, `Point` or `any`. A type
 * that was not written down is `any`, whether it's a parameter, a result or a field. The exception
 * is a top-level `let` or `const`, where the type the compiler worked out is recorded. Default
 * values and bodies are never recorded, only whether a default exists.
 *
 * Private members and private fields are not in the interface. A class that can be created from
 * outside has one `init` member that says how. Its parameters are the constructor arguments, which
 * are the `pub` fields with the parent's first, whether or not the class writes an `init` of its
 * own.
 */

struct LzChunk;

typedef struct LzInterface LzInterface;
typedef struct LzIfaceEntry LzIfaceEntry;
typedef struct LzIfaceMember LzIfaceMember;
typedef struct LzIfaceSig LzIfaceSig;

typedef enum LzIfaceKind {
    LZ_IFACE_FN,
    LZ_IFACE_CONST,
    LZ_IFACE_CLASS,
    LZ_IFACE_TRAIT,
    LZ_IFACE_ERROR,
    LZ_IFACE_TYPE /* an alias: `pub type Id = int` */
} LzIfaceKind;

typedef enum LzIfaceMemberKind {
    LZ_IFACE_MEMBER_INIT,     /* how to create an object: the constructor arguments */
    LZ_IFACE_MEMBER_FIELD,    /* `pub x: int`, or `pub read x: int` (see lz_interface_member_is_read_only) */
    LZ_IFACE_MEMBER_METHOD,   /* takes `self` first, and `self` is not listed among the parameters */
    LZ_IFACE_MEMBER_STATIC,   /* a function without `self`, called as Class.name() */
    LZ_IFACE_MEMBER_GETTER,   /* `get name(self)`, a property whose type is the result */
    LZ_IFACE_MEMBER_CONST,    /* `const NAME = ...` owned by the class */
    LZ_IFACE_MEMBER_OPERATOR  /* `operator +(self, other)`, where the name is the operator and `-@` is unary minus */
} LzIfaceMemberKind;

/*
 * The unit's interface, or NULL for a chunk that was not made by lz_compile. Never owned by the
 * caller.
 */
const LzInterface *lz_interface_of(const struct LzChunk *chunk);

size_t lz_interface_count(const LzInterface *iface);
/* Entry `index`, from 0 to count-1, in the order of the source. NULL when out of range. */
const LzIfaceEntry *lz_interface_entry(const LzInterface *iface, size_t index);

LzIfaceKind lz_interface_entry_kind(const LzIfaceEntry *entry);
/* "fn", "const", "class", "trait", "error" or "type". */
const char *lz_interface_kind_name(LzIfaceKind kind);
const char *lz_interface_entry_name(const LzIfaceEntry *entry);

/*
 * For let and const, the type of the variable. For type, the type the alias stands for. Else
 * NULL.
 */
const char *lz_interface_entry_type(const LzIfaceEntry *entry);
/* For fn, its signature. Else NULL. */
const LzIfaceSig *lz_interface_entry_sig(const LzIfaceEntry *entry);

/*
 * For class and error, the parent class. For an error that's the parent error kind, or `Error` when
 * none is written. Else NULL.
 */
const char *lz_interface_entry_parent(const LzIfaceEntry *entry);
/* For class and error, the traits in the head. For a trait, the traits it extends. */
size_t lz_interface_entry_trait_count(const LzIfaceEntry *entry);
const char *lz_interface_entry_trait(const LzIfaceEntry *entry, size_t index);

/*
 * For class, error and trait, the `pub` members in the order of the source. A class lists `init`
 * first.
 */
size_t lz_interface_entry_member_count(const LzIfaceEntry *entry);
const LzIfaceMember *lz_interface_entry_member(const LzIfaceEntry *entry, size_t index);

LzIfaceMemberKind lz_interface_member_kind(const LzIfaceMember *member);
const char *lz_interface_member_kind_name(LzIfaceMemberKind kind);
const char *lz_interface_member_name(const LzIfaceMember *member);

/* For field and const, its type. Else NULL. */
const char *lz_interface_member_type(const LzIfaceMember *member);
/* For init, method, static, getter and operator, the signature. Else NULL. */
const LzIfaceSig *lz_interface_member_sig(const LzIfaceMember *member);

/* A field with `pub read`, readable from anywhere but written only inside the class. */
bool lz_interface_member_is_read_only(const LzIfaceMember *member);
/* A field that has a default value. The value itself is not recorded. */
bool lz_interface_member_has_default(const LzIfaceMember *member);
/* trait member: written without a body, so a class using the trait must provide it. */
bool lz_interface_member_is_required(const LzIfaceMember *member);
/* override: written with `override`. */
bool lz_interface_member_is_override(const LzIfaceMember *member);
/*
 * The class writes an `init(self)` of its own. It runs after the fields are set and takes no
 * arguments, so the parameters of the member are the constructor's either way.
 */
bool lz_interface_member_has_explicit_init(const LzIfaceMember *member);

/* The parameters in order, with a variadic one (`*rest`) last. */
size_t lz_interface_sig_param_count(const LzIfaceSig *sig);
const char *lz_interface_sig_param_name(const LzIfaceSig *sig, size_t index);
/* The type of the parameter. For a variadic one, the type of each surplus argument. */
const char *lz_interface_sig_param_type(const LzIfaceSig *sig, size_t index);
bool lz_interface_sig_param_has_default(const LzIfaceSig *sig, size_t index);
bool lz_interface_sig_param_is_rest(const LzIfaceSig *sig, size_t index);
bool lz_interface_sig_is_variadic(const LzIfaceSig *sig);
/*
 * The type parameters of a generic function, in order. `fn first<T>(xs: T[]) -> T` has one, "T".
 * The parameter and result types mention them by name ("T[]", "T"). Types are written as text,
 * like `int[]`, `fn(int) -> bool` or `(fn(int) -> int)?`, where a function type with a result gets
 * parentheses when something follows it. A parameter with a default is not part of a function
 * type's text.
 */
size_t lz_interface_sig_type_param_count(const LzIfaceSig *sig);
const char *lz_interface_sig_type_param_name(const LzIfaceSig *sig, size_t index);
/*
 * The result type, which is the declared one, or else what the compiler worked out from the
 * function's `return`s, and `any` when it couldn't. lz_interface_sig_result_declared tells which.
 * An init has no result, so it's "nil".
 */
const char *lz_interface_sig_result(const LzIfaceSig *sig);
/* Was a result type written? */
bool lz_interface_sig_result_declared(const LzIfaceSig *sig);

#endif
