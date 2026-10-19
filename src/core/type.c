#include "lizard/type.h"

#include <stdio.h>
#include <string.h>

#define DESCRIBE_STRING_LIMIT 24

uint16_t lz_kind_of(const LzValue *value) {
    switch (value->type) {
    case LZ_VAL_NIL:
        return LZ_KIND_NIL;
    case LZ_VAL_BOOL:
        return LZ_KIND_BOOL;
    case LZ_VAL_INT:
        return LZ_KIND_INT;
    case LZ_VAL_FLOAT:
        return LZ_KIND_FLOAT;
    case LZ_VAL_OBJECT:
        return value->as.object->type == LZ_OBJ_STRING ? LZ_KIND_STRING : LZ_KIND_FUNCTION;
    }
    return 0;
}

const char *lz_kind_name(uint16_t kind) {
    switch (kind) {
    case LZ_KIND_NIL:
        return "nil";
    case LZ_KIND_BOOL:
        return "bool";
    case LZ_KIND_INT:
        return "int";
    case LZ_KIND_FLOAT:
        return "float";
    case LZ_KIND_STRING:
        return "string";
    case LZ_KIND_FUNCTION:
        return "function";
    default:
        return "value";
    }
}

LzType lz_type_of_kinds(uint16_t kinds) {
    LzType type;
    memset(&type, 0, sizeof type);
    type.kinds = kinds;
    return type;
}

static uint16_t literal_kind_bit(const LzLiteral *literal) {
    switch (literal->kind) {
    case LZ_VAL_INT:
        return LZ_KIND_INT;
    case LZ_VAL_BOOL:
        return LZ_KIND_BOOL;
    default:
        return LZ_KIND_STRING;
    }
}

static bool strings_equal(const LzString *a, const LzString *b) {
    return a == b || (a->length == b->length && memcmp(a->data, b->data, a->length) == 0);
}

static bool literals_equal(const LzLiteral *a, const LzLiteral *b) {
    if (a->kind != b->kind) {
        return false;
    }
    switch (a->kind) {
    case LZ_VAL_INT:
        return a->as.integer == b->as.integer;
    case LZ_VAL_BOOL:
        return a->as.boolean == b->as.boolean;
    default:
        return strings_equal(a->as.string, b->as.string);
    }
}

static bool literal_matches(const LzLiteral *literal, const LzValue *value) {
    if (literal->kind != value->type) {
        return false;
    }
    switch (literal->kind) {
    case LZ_VAL_INT:
        return literal->as.integer == value->as.integer;
    case LZ_VAL_BOOL:
        return literal->as.boolean == value->as.boolean;
    default:
        return value->as.object->type == LZ_OBJ_STRING &&
               strings_equal(literal->as.string, (const LzString *)value->as.object);
    }
}

static bool literal_from_value(const LzValue *value, LzLiteral *out) {
    switch (value->type) {
    case LZ_VAL_INT:
        out->kind = LZ_VAL_INT;
        out->as.integer = value->as.integer;
        return true;
    case LZ_VAL_BOOL:
        out->kind = LZ_VAL_BOOL;
        out->as.boolean = value->as.boolean;
        return true;
    case LZ_VAL_OBJECT:
        if (value->as.object->type == LZ_OBJ_STRING) {
            out->kind = LZ_VAL_OBJECT;
            out->as.string = (const LzString *)value->as.object;
            return true;
        }
        return false;
    default:
        return false;
    }
}

LzType lz_type_of_value(const LzValue *value) {
    LzType type = lz_type_of_kinds(0);
    LzLiteral literal;
    if (literal_from_value(value, &literal)) {
        type.literals[0] = literal;
        type.literal_count = 1;
    } else {
        type.kinds = lz_kind_of(value);
    }
    return type;
}

uint16_t lz_type_possible_kinds(const LzType *type) {
    uint16_t kinds = type->kinds;
    for (size_t i = 0; i < type->literal_count; i++) {
        kinds = (uint16_t)(kinds | literal_kind_bit(&type->literals[i]));
    }
    return kinds;
}

LzType lz_type_widen(const LzType *type) {
    return lz_type_of_kinds(lz_type_possible_kinds(type));
}

static bool literal_accepted(const LzType *type, const LzLiteral *literal) {
    if ((literal_kind_bit(literal) & type->kinds) != 0) {
        return true;
    }
    for (size_t i = 0; i < type->literal_count; i++) {
        if (literals_equal(&type->literals[i], literal)) {
            return true;
        }
    }
    return false;
}

bool lz_type_add_literal(LzType *type, const LzLiteral *literal) {
    if (literal_accepted(type, literal)) {
        return true;
    }
    if (type->literal_count >= LZ_TYPE_MAX_LITERALS) {
        return false;
    }
    type->literals[type->literal_count++] = *literal;
    return true;
}

bool lz_type_union(LzType *out, const LzType *a, const LzType *b) {
    LzType result = lz_type_of_kinds((uint16_t)(a->kinds | b->kinds));
    for (size_t i = 0; i < a->literal_count; i++) {
        if (!lz_type_add_literal(&result, &a->literals[i])) {
            return false;
        }
    }
    for (size_t i = 0; i < b->literal_count; i++) {
        if (!lz_type_add_literal(&result, &b->literals[i])) {
            return false;
        }
    }
    *out = result;
    return true;
}

bool lz_type_equal(const LzType *a, const LzType *b) {
    if (a->kinds != b->kinds || a->literal_count != b->literal_count) {
        return false;
    }
    for (size_t i = 0; i < a->literal_count; i++) {
        if (!literal_accepted(b, &a->literals[i])) {
            return false;
        }
    }
    return true;
}

static uint32_t mix(uint64_t value) {
    value ^= value >> 33;
    value *= 0xFF51AFD7ED558CCDull;
    value ^= value >> 33;
    value *= 0xC4CEB9FE1A85EC53ull;
    value ^= value >> 33;
    return (uint32_t)value;
}

uint32_t lz_type_hash(const LzType *type) {
    uint32_t hash = mix(type->kinds + 1u);
    for (size_t i = 0; i < type->literal_count; i++) {
        const LzLiteral *literal = &type->literals[i];
        uint32_t part;
        switch (literal->kind) {
        case LZ_VAL_INT:
            part = mix((uint64_t)literal->as.integer);
            break;
        case LZ_VAL_BOOL:
            part = mix(literal->as.boolean ? 0x1111u : 0x2222u);
            break;
        default:
            part = literal->as.string->hash ^ 0x5BD1E995u;
            break;
        }
        hash += part; /* a sum, so the order of the literals does not matter */
    }
    return hash;
}

bool lz_type_matches(const LzType *type, const LzValue *value) {
    if ((lz_kind_of(value) & type->kinds) != 0) {
        return true;
    }
    for (size_t i = 0; i < type->literal_count; i++) {
        if (literal_matches(&type->literals[i], value)) {
            return true;
        }
    }
    return false;
}

LzAssign lz_type_assignable(const LzType *from, const LzType *to) {
    bool always = (from->kinds & ~to->kinds & LZ_KINDS_ANY) == 0;
    bool overlap = (from->kinds & to->kinds) != 0;

    for (size_t i = 0; i < from->literal_count; i++) {
        if (literal_accepted(to, &from->literals[i])) {
            overlap = true;
        } else {
            always = false;
        }
    }
    if (always) {
        return LZ_ASSIGN_ALWAYS;
    }

    for (size_t i = 0; i < to->literal_count; i++) {
        if ((literal_kind_bit(&to->literals[i]) & from->kinds) != 0) {
            overlap = true;
        }
    }
    return overlap ? LZ_ASSIGN_MAYBE : LZ_ASSIGN_NEVER;
}

static void write_text(const LzWriter *out, const char *text) {
    (void)lz_writer_write_cstr(out, text);
}

static void write_string(const LzWriter *out, const LzString *string) {
    size_t shown = string->length < DESCRIBE_STRING_LIMIT ? string->length : DESCRIBE_STRING_LIMIT;
    write_text(out, "\"");
    (void)lz_writer_write(out, string->data, shown);
    if (shown < string->length) {
        write_text(out, "...");
    }
    write_text(out, "\"");
}

static void write_literal(const LzWriter *out, const LzLiteral *literal) {
    char text[32];
    switch (literal->kind) {
    case LZ_VAL_INT:
        (void)snprintf(text, sizeof text, "%lld", (long long)literal->as.integer);
        write_text(out, text);
        break;
    case LZ_VAL_BOOL:
        write_text(out, literal->as.boolean ? "true" : "false");
        break;
    default:
        write_string(out, literal->as.string);
        break;
    }
}

void lz_type_describe(const LzType *type, const LzWriter *out) {
    if (type->kinds == LZ_KINDS_ANY) {
        write_text(out, "any");
        return;
    }
    if (type->kinds == 0 && type->literal_count == 0) {
        write_text(out, "never");
        return;
    }

    bool first = true;
    uint16_t kinds = type->kinds;
#define PART(text)                                                                                 \
    do {                                                                                           \
        if (!first) {                                                                              \
            write_text(out, " | ");                                                                \
        }                                                                                          \
        first = false;                                                                             \
        write_text(out, text);                                                                     \
    } while (0)

    if ((kinds & LZ_KINDS_NUMBER) == LZ_KINDS_NUMBER) {
        PART("number");
        kinds = (uint16_t)(kinds & ~LZ_KINDS_NUMBER);
    }
    if ((kinds & LZ_KIND_INT) != 0) {
        PART("int");
    }
    if ((kinds & LZ_KIND_FLOAT) != 0) {
        PART("float");
    }
    if ((kinds & LZ_KIND_STRING) != 0) {
        PART("string");
    }
    if ((kinds & LZ_KIND_BOOL) != 0) {
        PART("bool");
    }
    if ((kinds & LZ_KIND_NIL) != 0) {
        PART("nil");
    }
    if ((kinds & LZ_KIND_FUNCTION) != 0) {
        PART("function");
    }
#undef PART

    for (size_t i = 0; i < type->literal_count; i++) {
        if (!first) {
            write_text(out, " | ");
        }
        first = false;
        write_literal(out, &type->literals[i]);
    }
}

void lz_value_describe(const LzValue *value, const LzWriter *out) {
    LzLiteral literal;
    if (literal_from_value(value, &literal)) {
        write_literal(out, &literal);
    } else if (value->type == LZ_VAL_OBJECT) {
        write_text(out, "function");
    } else {
        (void)lz_value_write(value, out);
    }
}
