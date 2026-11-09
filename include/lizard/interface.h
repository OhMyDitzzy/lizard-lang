#ifndef LIZARD_INTERFACE_H
#define LIZARD_INTERFACE_H

#include <stdbool.h>
#include <stddef.h>

/*
 * The interface of a compiled unit: what its `pub` declarations offer to other code.
 *
 * Without modules `pub` changes nothing at run time, but the compiler writes down every top-level
 * `pub` declaration, in source order, with its signature. A tool that links against the Lizard
 * library (a stub generator, say) reads that record through the functions below. Everything here
 * is read-only and every pointer stays valid until the unit's chunk is released; the structures
 * themselves are opaque, so nothing of the compiler is exposed.
 *
 * Types are written as Lizard writes them: `int`, `string?`, `int[]`, `Point`, `any`. A type that
 * was not written down is `any` (a parameter, a result, a field), except for a top-level `let` or
 * `const`, where the type the compiler worked out is recorded. Default values and bodies are never
 * recorded, only whether a default exists.
 *
 * Private members and private fields are not in the interface. A class that can be created from
 * outside has one `init` member that says how: its parameters are the constructor arguments, which
 * are the `pub` fields (the parent's first), whether or not the class writes an `init` of its own.
 */

struct LzChunk;

typedef struct LzInterface LzInterface;
typedef struct LzIfaceEntry LzIfaceEntry;
typedef struct LzIfaceMember LzIfaceMember;
typedef struct LzIfaceSig LzIfaceSig;

typedef enum LzIfaceKind {
    LZ_IFACE_FN,
    LZ_IFACE_CONST,
    LZ_IFACE_LET,
    LZ_IFACE_CLASS,
    LZ_IFACE_TRAIT,
    LZ_IFACE_ERROR,
    LZ_IFACE_TYPE /* an alias: `pub type Id = int` */
} LzIfaceKind;

typedef enum LzIfaceMemberKind {
    LZ_IFACE_MEMBER_INIT,     /* how to create an object: the constructor arguments */
    LZ_IFACE_MEMBER_FIELD,    /* `pub x: int`, or `pub read x: int` (see lz_interface_member_is_read_only) */
    LZ_IFACE_MEMBER_METHOD,   /* takes `self` first; `self` is not listed among the parameters */
    LZ_IFACE_MEMBER_STATIC,   /* a function without `self`, called as Class.name() */
    LZ_IFACE_MEMBER_GETTER,   /* `get name(self)`: a property, its type is the result */
    LZ_IFACE_MEMBER_CONST,    /* `const NAME = ...` owned by the class */
    LZ_IFACE_MEMBER_OPERATOR  /* `operator +(self, other)`: the name is the operator, `-@` for unary minus */
} LzIfaceMemberKind;

/* The unit's interface, or NULL for a chunk that was not made by lz_compile. Never owned by the caller. */
const LzInterface *lz_interface_of(const struct LzChunk *chunk);

/* Entries ------------------------------------------------------------------------------------ */

size_t lz_interface_count(const LzInterface *iface);
/* Entry `index` (0 .. count-1), in the order of the source; NULL out of range. */
const LzIfaceEntry *lz_interface_entry(const LzInterface *iface, size_t index);

LzIfaceKind lz_interface_entry_kind(const LzIfaceEntry *entry);
/* "fn", "const", "let", "class", "trait", "error" or "type". */
const char *lz_interface_kind_name(LzIfaceKind kind);
const char *lz_interface_entry_name(const LzIfaceEntry *entry);

/* let, const: the type of the variable. type: the type the alias stands for. Else NULL. */
const char *lz_interface_entry_type(const LzIfaceEntry *entry);
/* fn: its signature. Else NULL. */
const LzIfaceSig *lz_interface_entry_sig(const LzIfaceEntry *entry);

/* class, error: the parent class (an error's parent error kind, `Error` when none is written); else NULL. */
const char *lz_interface_entry_parent(const LzIfaceEntry *entry);
/* class, error: the traits in the head. trait: the traits it extends. */
size_t lz_interface_entry_trait_count(const LzIfaceEntry *entry);
const char *lz_interface_entry_trait(const LzIfaceEntry *entry, size_t index);

/* class, error, trait: the `pub` members in the order of the source (a class lists `init` first). */
size_t lz_interface_entry_member_count(const LzIfaceEntry *entry);
const LzIfaceMember *lz_interface_entry_member(const LzIfaceEntry *entry, size_t index);

/* Members ------------------------------------------------------------------------------------ */

LzIfaceMemberKind lz_interface_member_kind(const LzIfaceMember *member);
const char *lz_interface_member_kind_name(LzIfaceMemberKind kind);
const char *lz_interface_member_name(const LzIfaceMember *member);

/* field, const: its type. Else NULL. */
const char *lz_interface_member_type(const LzIfaceMember *member);
/* init, method, static, getter, operator: the signature. Else NULL. */
const LzIfaceSig *lz_interface_member_sig(const LzIfaceMember *member);

/* field: `pub read`, readable from anywhere but written only inside the class. */
bool lz_interface_member_is_read_only(const LzIfaceMember *member);
/* field: has a default value (the value itself is not recorded). */
bool lz_interface_member_has_default(const LzIfaceMember *member);
/* trait member: written without a body, so a class using the trait must provide it. */
bool lz_interface_member_is_required(const LzIfaceMember *member);
/* override: written with `override`. */
bool lz_interface_member_is_override(const LzIfaceMember *member);
/* init: the class writes an `init(self)` of its own (it runs after the fields are set and takes no
 * arguments, so the parameters of the member are the constructor's either way). */
bool lz_interface_member_has_explicit_init(const LzIfaceMember *member);

/* Signatures --------------------------------------------------------------------------------- */

/* The parameters in order, a variadic one (`*rest`) last. */
size_t lz_interface_sig_param_count(const LzIfaceSig *sig);
const char *lz_interface_sig_param_name(const LzIfaceSig *sig, size_t index);
/* The type of the parameter; of a variadic one, the type of each surplus argument. */
const char *lz_interface_sig_param_type(const LzIfaceSig *sig, size_t index);
bool lz_interface_sig_param_has_default(const LzIfaceSig *sig, size_t index);
bool lz_interface_sig_param_is_rest(const LzIfaceSig *sig, size_t index);
bool lz_interface_sig_is_variadic(const LzIfaceSig *sig);
/* The declared result type, `any` when none is written. An init has no result: "nil". */
const char *lz_interface_sig_result(const LzIfaceSig *sig);
/* Was a result type written? */
bool lz_interface_sig_result_declared(const LzIfaceSig *sig);

#endif
