#ifndef LIZARD_TYPE_H
#define LIZARD_TYPE_H

#include "lizard/value.h"
#include "lizard/writer.h"

/* One bit per kind of runtime value. */
typedef enum LzKind {
    LZ_KIND_NIL = 1 << 0,
    LZ_KIND_BOOL = 1 << 1,
    LZ_KIND_INT = 1 << 2,
    LZ_KIND_FLOAT = 1 << 3,
    LZ_KIND_STRING = 1 << 4,
    LZ_KIND_FUNCTION = 1 << 5,
    LZ_KIND_ARRAY = 1 << 6,
    LZ_KIND_DICT = 1 << 7,
    LZ_KIND_ERROR = 1 << 8,
    LZ_KIND_INSTANCE = 1 << 9 /* an object made by a class, and its kind name is the class name */
} LzKind;

#define LZ_KINDS_NUMBER (LZ_KIND_INT | LZ_KIND_FLOAT)
#define LZ_KINDS_ANY                                                                               \
    (LZ_KIND_NIL | LZ_KIND_BOOL | LZ_KIND_INT | LZ_KIND_FLOAT | LZ_KIND_STRING | LZ_KIND_FUNCTION |   \
     LZ_KIND_ARRAY | LZ_KIND_DICT | LZ_KIND_ERROR | LZ_KIND_INSTANCE)

#define LZ_TYPE_MAX_LITERALS 8

struct LzType;

/*
 * The signature a function type carries, like `fn(int, string) -> bool`. Like the item type of an
 * array it's interned by the VM (lz_vm_fn_of), and every type it points at is interned too, so two
 * equal signatures are the same pointer and comparing them is comparing pointers.
 *
 * A NULL type pointer means `any`. A NULL `result` is a result nobody cares about, like `fn(int)`.
 * The callers get `any` back and any function result is accepted. The surplus arguments of a rest
 * parameter (has_rest) all have type `rest`.
 */
typedef struct LzFnType {
    uint16_t param_count; /* positional parameters, not counting the rest parameter */
    uint16_t required;    /* the first `required` of them have no default */
    bool has_rest;
    const struct LzType *result;
    const struct LzType *rest;
    const struct LzType *params[]; /* param_count entries */
} LzFnType;

/* A single value that a type accepts: an int, a bool or a string. */
typedef struct LzLiteral {
    LzValueType kind; /* LZ_VAL_INT, LZ_VAL_BOOL, or LZ_VAL_OBJECT for a string */
    union {
        int64_t integer;
        bool boolean;
        const LzString *string;
    } as;
} LzLiteral;

/*
 * A type is the set of values it accepts, which is every value whose kind is in kinds plus the
 * listed literals. `int | nil` is two kinds, `"r" | "w"` is two literals, `any` is every kind and
 * `never` is empty. String literals point at interned strings, which live as long as the VM.
 */
typedef struct LzType {
    uint16_t kinds;
    uint8_t literal_count;
    uint8_t tparam_nullable; /* with tparam, the type is `T?`, meaning T or nil */
    LzLiteral literals[LZ_TYPE_MAX_LITERALS];
    /*
     * Only meaningful when kinds has LZ_KIND_ARRAY. It's the type of the items, or NULL for any. It
     * points at a type interned by the VM (lz_vm_array_of), so two equal element types are the same
     * pointer.
     *
     * For LZ_KIND_DICT, element is the key type and value_element the value type, and both are NULL
     * for a bare `dict`. Interned by lz_vm_dict_of.
     */
    const struct LzType *element;
    const struct LzType *value_element;
    /*
     * Only meaningful when kinds has LZ_KIND_INSTANCE. It's the interned name of the class the
     * value must be an instance of, or NULL for an instance of any class, which is what `any`
     * accepts.
     */
    const LzString *class_name;
    /*
     * With class_name, the identity of that class (LzClass.id), or 0 for an instance of any class.
     * Two classes with one name, like a class defined again, have different ids and never match
     * each other. It's a number and never dereferenced, so a class that is gone can't dangle here.
     */
    uint64_t class_id;
    /*
     * Only meaningful when kinds has LZ_KIND_FUNCTION. It's the signature every function in the
     * type must fit, or NULL for any function. Interned by lz_vm_fn_of. At run time only the number
     * of arguments is checked against it, because a function value doesn't carry parameter types.
     */
    const LzFnType *fn;
    /*
     * A type parameter of a generic function, like `T` in `fn first<T>(xs: T[]) -> T`, as the
     * interned name. Such a type is `any` everywhere, and exists so that a call can work out what T
     * stands for. The compiler erases it before a type reaches a run-time check, see subst_tparams
     * in the compiler. Written `T?` it has tparam_nullable set, because the union with nil would
     * otherwise forget T.
     */
    const LzString *tparam;
} LzType;

typedef enum LzAssign {
    LZ_ASSIGN_ALWAYS, /* every value of the source type is accepted */
    LZ_ASSIGN_MAYBE,  /* some are, so a run-time check is needed */
    LZ_ASSIGN_NEVER   /* none is, so it's a compile-time error */
} LzAssign;

uint16_t lz_kind_of(const LzValue *value);
const char *lz_kind_name(uint16_t kind);
/*
 * The name of a value's kind in messages, which is the class name for an instance and else
 * lz_kind_name.
 */
const char *lz_value_kind_name(const LzValue *value);

LzType lz_type_of_kinds(uint16_t kinds);

/* The exact type of one value: a literal for int, bool and string, a kind otherwise. */
LzType lz_type_of_value(const LzValue *value);

/* Every kind the type can hold, counting the kinds of its literals. */
uint16_t lz_type_possible_kinds(const LzType *type);

/* The same kinds with no literals: `"r" | "w"` becomes `string`. */
LzType lz_type_widen(const LzType *type);

/* False when the union would need more than LZ_TYPE_MAX_LITERALS literals. */
bool lz_type_add_literal(LzType *type, const LzLiteral *literal);
/*
 * Two different array types in a union merge into an array of any, so `int[] | string[]` becomes
 * `any[]`. That's a safe over-approximation for the type of an expression, and the compiler rejects
 * such a union in a written type.
 */
bool lz_type_union(LzType *out, const LzType *a, const LzType *b);

bool lz_type_equal(const LzType *a, const LzType *b);

/* `any` has every kind, no literals, and nothing narrower said about arrays, dicts or functions. */
bool lz_type_is_any(const LzType *type);

/*
 * Can a function value take `argc` arguments? A closure knows its parameters and a native its
 * limits. For a class, or a method of unknown shape, the answer is yes and the call itself checks.
 */
bool lz_function_value_takes(const LzValue *value, size_t argc);

/* Equal types hash equally, whatever the order of their literals. */
uint32_t lz_type_hash(const LzType *type);
bool lz_type_matches(const LzType *type, const LzValue *value);

/* Like lz_type_matches, but a link cell is tested by its target. Only for the `is` operator. */
bool lz_type_matches_through_link(const LzType *type, const LzValue *value);
LzAssign lz_type_assignable(const LzType *from, const LzType *to);

/* "int | string", "number", "any", "never", "\"r\" | \"w\"" */
void lz_type_describe(const LzType *type, const LzWriter *out);

/* The value itself for short values: 5, true, "abc". */
void lz_value_describe(const LzValue *value, const LzWriter *out);

#endif
