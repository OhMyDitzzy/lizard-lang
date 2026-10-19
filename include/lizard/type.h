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
    LZ_KIND_FUNCTION = 1 << 5
} LzKind;

#define LZ_KINDS_NUMBER (LZ_KIND_INT | LZ_KIND_FLOAT)
#define LZ_KINDS_ANY                                                                               \
    (LZ_KIND_NIL | LZ_KIND_BOOL | LZ_KIND_INT | LZ_KIND_FLOAT | LZ_KIND_STRING | LZ_KIND_FUNCTION)

#define LZ_TYPE_MAX_LITERALS 8

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
 * A type is the set of values it accepts: every value whose kind is in kinds,
 * plus the listed literals. `int | nil` is two kinds, `"r" | "w"` is two
 * literals, `any` is every kind and `never` is empty. String literals point
 * at interned strings, which live as long as the VM.
 */
typedef struct LzType {
    uint16_t kinds;
    uint8_t literal_count;
    LzLiteral literals[LZ_TYPE_MAX_LITERALS];
} LzType;

typedef enum LzAssign {
    LZ_ASSIGN_ALWAYS, /* every value of the source type is accepted */
    LZ_ASSIGN_MAYBE,  /* some are: a run-time check is needed */
    LZ_ASSIGN_NEVER   /* none is: a compile-time error */
} LzAssign;

uint16_t lz_kind_of(const LzValue *value);
const char *lz_kind_name(uint16_t kind);

LzType lz_type_of_kinds(uint16_t kinds);

/* The exact type of one value: a literal for int, bool and string, a kind otherwise. */
LzType lz_type_of_value(const LzValue *value);

/* Every kind the type can hold, counting the kinds of its literals. */
uint16_t lz_type_possible_kinds(const LzType *type);

/* The same kinds with no literals: `"r" | "w"` becomes `string`. */
LzType lz_type_widen(const LzType *type);

/* False when the union would need more than LZ_TYPE_MAX_LITERALS literals. */
bool lz_type_add_literal(LzType *type, const LzLiteral *literal);
bool lz_type_union(LzType *out, const LzType *a, const LzType *b);

bool lz_type_equal(const LzType *a, const LzType *b);

/* Equal types hash equally, whatever the order of their literals. */
uint32_t lz_type_hash(const LzType *type);
bool lz_type_matches(const LzType *type, const LzValue *value);
LzAssign lz_type_assignable(const LzType *from, const LzType *to);

/* "int | string", "number", "any", "never", "\"r\" | \"w\"" */
void lz_type_describe(const LzType *type, const LzWriter *out);

/* The value itself for short values: 5, true, "abc". */
void lz_value_describe(const LzValue *value, const LzWriter *out);

#endif
