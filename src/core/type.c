#include "lizard/type.h"

#include "lizard/array.h"

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
        switch (value->as.object->type) {
        case LZ_OBJ_STRING:
            return LZ_KIND_STRING;
        case LZ_OBJ_ARRAY:
            return LZ_KIND_ARRAY;
        case LZ_OBJ_NATIVE:
        case LZ_OBJ_CLOSURE:
            return LZ_KIND_FUNCTION;
        case LZ_OBJ_FUNCTION:
        case LZ_OBJ_UPVALUE:
            return 0; /* internal objects are never values a script can hold */
        }
        return 0;
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
    case LZ_KIND_ARRAY:
        return "array";
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
        if (lz_value_is_array(value)) {
            type.element = lz_value_as_array(value)->element;
        }
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

bool lz_type_closure_free(const LzType *type) {
    /* `any` (NULL) can hold a closure, so can a function; an array type can when its items can. */
    for (unsigned depth = 0; depth < 16; depth++) {
        if (type == NULL) {
            return false;
        }
        uint16_t kinds = lz_type_possible_kinds(type);
        if ((kinds & LZ_KIND_FUNCTION) != 0) {
            return false;
        }
        if ((kinds & LZ_KIND_ARRAY) == 0) {
            return true;
        }
        type = type->element;
    }
    return false; /* nested deeper than any program writes: say nothing rather than guess */
}

LzType lz_type_widen(const LzType *type) {
    LzType widened = lz_type_of_kinds(lz_type_possible_kinds(type));
    widened.element = type->element;
    return widened;
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
    if ((a->kinds & LZ_KIND_ARRAY) != 0 && (b->kinds & LZ_KIND_ARRAY) != 0) {
        result.element = a->element == b->element ? a->element : NULL;
    } else if ((a->kinds & LZ_KIND_ARRAY) != 0) {
        result.element = a->element;
    } else if ((b->kinds & LZ_KIND_ARRAY) != 0) {
        result.element = b->element;
    }
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
    if ((a->kinds & LZ_KIND_ARRAY) != 0 && a->element != b->element) {
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
    if ((type->kinds & LZ_KIND_ARRAY) != 0) {
        hash ^= mix((uint64_t)(uintptr_t)type->element);
    }
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
    if (lz_value_is_array(value)) {
        if ((type->kinds & LZ_KIND_ARRAY) == 0) {
            return false;
        }
        const LzArray *array = lz_value_as_array(value);
        const LzType *wanted = type->element;
        if (wanted == NULL || array->element == wanted) {
            return true;
        }
        if (array->element != NULL && lz_type_assignable(array->element, wanted) == LZ_ASSIGN_ALWAYS) {
            return true; /* every item already fits the tag, and the tag fits the target */
        }
        for (size_t i = 0; i < array->count; i++) {
            if (!lz_type_matches(wanted, &array->items[i])) {
                return false;
            }
        }
        return true;
    }
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

/* How an array of items `from` can be stored as an array of items `to` (NULL = any). */
static LzAssign element_assignable(const LzType *from, const LzType *to) {
    if (to == NULL) {
        return LZ_ASSIGN_ALWAYS;
    }
    if (from == NULL) {
        LzType any = lz_type_of_kinds(LZ_KINDS_ANY);
        return lz_type_assignable(&any, to);
    }
    return lz_type_assignable(from, to);
}

LzAssign lz_type_assignable(const LzType *from, const LzType *to) {
    uint16_t scalar = (uint16_t)~LZ_KIND_ARRAY;
    bool always = (from->kinds & ~to->kinds & LZ_KINDS_ANY & scalar) == 0;
    bool overlap = (from->kinds & to->kinds & scalar) != 0;

    if ((from->kinds & LZ_KIND_ARRAY) != 0) {
        if ((to->kinds & LZ_KIND_ARRAY) == 0) {
            always = false;
        } else {
            LzAssign items = element_assignable(from->element, to->element);
            if (items != LZ_ASSIGN_ALWAYS) {
                always = false;
            }
            if (items != LZ_ASSIGN_NEVER) {
                overlap = true;
            }
        }
    }

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
    if (type->kinds == LZ_KINDS_ANY && type->element == NULL) {
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
    if ((kinds & LZ_KIND_ARRAY) != 0) {
        if (!first) {
            write_text(out, " | ");
        }
        first = false;
        if (type->element == NULL) {
            write_text(out, "any[]");
        } else {
            /* A union item type needs parentheses: (int | string)[]. */
            LzBuffer item = {0};
            LzWriter item_out = lz_writer_from_buffer(&item);
            lz_type_describe(type->element, &item_out);
            bool compound = item.data != NULL && strstr(item.data, " | ") != NULL;
            write_text(out, compound ? "(" : "");
            write_text(out, item.data != NULL ? item.data : "?");
            write_text(out, compound ? ")[]" : "[]");
            lz_buffer_free(&item);
        }
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
        write_text(out, lz_value_is_array(value) ? "array" : "function");
    } else {
        (void)lz_value_write(value, out);
    }
}
