#include <string.h>

#include "lizard/type.h"
#include "test_util.h"

static LzHeap heap;

static LzLiteral int_literal(int64_t value) {
    LzLiteral literal = {.kind = LZ_VAL_INT, .as.integer = value};
    return literal;
}

static LzLiteral bool_literal(bool value) {
    LzLiteral literal = {.kind = LZ_VAL_BOOL, .as.boolean = value};
    return literal;
}

static LzLiteral string_literal(const char *text) {
    LzLiteral literal = {.kind = LZ_VAL_OBJECT};
    literal.as.string = lz_string_new(&heap, text, strlen(text));
    return literal;
}

static LzType kinds(uint16_t value) {
    return lz_type_of_kinds(value);
}

static LzType with_literals(uint16_t value, const LzLiteral *items, size_t count) {
    LzType type = lz_type_of_kinds(value);
    for (size_t i = 0; i < count; i++) {
        LZ_CHECK(lz_type_add_literal(&type, &items[i]));
    }
    return type;
}

static LzType modes(void) {
    LzLiteral items[] = {string_literal("r"), string_literal("w")};
    return with_literals(0, items, 2);
}

static LzType numbers_12(void) {
    LzLiteral items[] = {int_literal(1), int_literal(2)};
    return with_literals(0, items, 2);
}

static void check_assign(const LzType *from, const LzType *to, LzAssign expected) {
    LZ_CHECK(lz_type_assignable(from, to) == expected);
}

static bool described_as(const LzType *type, const char *expected) {
    LzBuffer buffer = {0};
    LzWriter writer = lz_writer_from_buffer(&buffer);
    lz_type_describe(type, &writer);
    bool same = buffer.data != NULL && strcmp(buffer.data, expected) == 0;
    if (!same) {
        fprintf(stderr, "described [%s], expected [%s]\n", buffer.data != NULL ? buffer.data : "",
                expected);
    }
    lz_buffer_free(&buffer);
    return same;
}

static bool value_described_as(const LzValue *value, const char *expected) {
    LzBuffer buffer = {0};
    LzWriter writer = lz_writer_from_buffer(&buffer);
    lz_value_describe(value, &writer);
    bool same = buffer.data != NULL && strcmp(buffer.data, expected) == 0;
    lz_buffer_free(&buffer);
    return same;
}

static void test_kinds(void) {
    LzValue nil = lz_value_nil();
    LzValue yes = lz_value_bool(true);
    LzValue five = lz_value_int(5);
    LzValue half = lz_value_float(0.5);
    LZ_CHECK(lz_kind_of(&nil) == LZ_KIND_NIL);
    LZ_CHECK(lz_kind_of(&yes) == LZ_KIND_BOOL);
    LZ_CHECK(lz_kind_of(&five) == LZ_KIND_INT);
    LZ_CHECK(lz_kind_of(&half) == LZ_KIND_FLOAT);

    LzString *text = lz_string_new(&heap, "x", 1);
    LzValue string = lz_value_object(&text->base);
    LZ_CHECK(lz_kind_of(&string) == LZ_KIND_STRING);

    LZ_CHECK(strcmp(lz_kind_name(LZ_KIND_INT), "int") == 0);
    LZ_CHECK(strcmp(lz_kind_name(LZ_KIND_FUNCTION), "function") == 0);
}

static void test_matching(void) {
    LzValue five = lz_value_int(5);
    LzValue half = lz_value_float(0.5);
    LzValue nil = lz_value_nil();
    LzString *r = lz_string_new(&heap, "r", 1);
    LzString *x = lz_string_new(&heap, "x", 1);
    LzValue r_value = lz_value_object(&r->base);
    LzValue x_value = lz_value_object(&x->base);

    LzType integer = kinds(LZ_KIND_INT);
    LzType number = kinds(LZ_KINDS_NUMBER);
    LzType nullable = kinds(LZ_KIND_INT | LZ_KIND_NIL);
    LZ_CHECK(lz_type_matches(&integer, &five));
    LZ_CHECK(!lz_type_matches(&integer, &half));
    LZ_CHECK(lz_type_matches(&number, &half));
    LZ_CHECK(lz_type_matches(&nullable, &nil));
    LZ_CHECK(!lz_type_matches(&integer, &nil));

    LzType mode = modes();
    LZ_CHECK(lz_type_matches(&mode, &r_value));
    LZ_CHECK(!lz_type_matches(&mode, &x_value));
    LZ_CHECK(!lz_type_matches(&mode, &five));

    LzType one_or_two = numbers_12();
    LzValue two = lz_value_int(2);
    LzValue three = lz_value_int(3);
    LZ_CHECK(lz_type_matches(&one_or_two, &two));
    LZ_CHECK(!lz_type_matches(&one_or_two, &three));

    LzLiteral only_true[] = {bool_literal(true)};
    LzType truthy = with_literals(0, only_true, 1);
    LzValue yes = lz_value_bool(true);
    LzValue no = lz_value_bool(false);
    LZ_CHECK(lz_type_matches(&truthy, &yes));
    LZ_CHECK(!lz_type_matches(&truthy, &no));

    LzType never = kinds(0);
    LZ_CHECK(!lz_type_matches(&never, &five));
    LzType any = kinds(LZ_KINDS_ANY);
    LZ_CHECK(lz_type_matches(&any, &nil) && lz_type_matches(&any, &r_value));
}

static void test_assignability(void) {
    LzType integer = kinds(LZ_KIND_INT);
    LzType floating = kinds(LZ_KIND_FLOAT);
    LzType string = kinds(LZ_KIND_STRING);
    LzType number = kinds(LZ_KINDS_NUMBER);
    LzType any = kinds(LZ_KINDS_ANY);
    LzType never = kinds(0);
    LzType nil = kinds(LZ_KIND_NIL);
    LzType nullable = kinds(LZ_KIND_INT | LZ_KIND_NIL);

    check_assign(&integer, &integer, LZ_ASSIGN_ALWAYS);
    check_assign(&integer, &number, LZ_ASSIGN_ALWAYS);
    check_assign(&number, &integer, LZ_ASSIGN_MAYBE);
    check_assign(&string, &integer, LZ_ASSIGN_NEVER);
    check_assign(&integer, &floating, LZ_ASSIGN_NEVER);
    check_assign(&floating, &integer, LZ_ASSIGN_NEVER);
    check_assign(&any, &integer, LZ_ASSIGN_MAYBE);
    check_assign(&integer, &any, LZ_ASSIGN_ALWAYS);
    check_assign(&any, &any, LZ_ASSIGN_ALWAYS);
    check_assign(&never, &integer, LZ_ASSIGN_ALWAYS);
    check_assign(&integer, &never, LZ_ASSIGN_NEVER);
    check_assign(&nil, &integer, LZ_ASSIGN_NEVER);
    check_assign(&nil, &nullable, LZ_ASSIGN_ALWAYS);
    check_assign(&nullable, &integer, LZ_ASSIGN_MAYBE);
    check_assign(&integer, &nullable, LZ_ASSIGN_ALWAYS);

    LzType mode = modes();
    LzLiteral r_item[] = {string_literal("r")};
    LzLiteral x_item[] = {string_literal("x")};
    LzType r = with_literals(0, r_item, 1);
    LzType x = with_literals(0, x_item, 1);
    check_assign(&r, &mode, LZ_ASSIGN_ALWAYS);
    check_assign(&x, &mode, LZ_ASSIGN_NEVER);
    check_assign(&string, &mode, LZ_ASSIGN_MAYBE);
    check_assign(&mode, &string, LZ_ASSIGN_ALWAYS);
    check_assign(&mode, &integer, LZ_ASSIGN_NEVER);
    check_assign(&mode, &any, LZ_ASSIGN_ALWAYS);

    LzType one_or_two = numbers_12();
    LzLiteral one_item[] = {int_literal(1)};
    LzLiteral three_item[] = {int_literal(3)};
    LzLiteral two_three[] = {int_literal(2), int_literal(3)};
    LzType one = with_literals(0, one_item, 1);
    LzType three = with_literals(0, three_item, 1);
    LzType later = with_literals(0, two_three, 2);
    check_assign(&one, &integer, LZ_ASSIGN_ALWAYS);
    check_assign(&one, &one_or_two, LZ_ASSIGN_ALWAYS);
    check_assign(&three, &one_or_two, LZ_ASSIGN_NEVER);
    check_assign(&integer, &one_or_two, LZ_ASSIGN_MAYBE);
    check_assign(&one_or_two, &later, LZ_ASSIGN_MAYBE);
    check_assign(&one_or_two, &integer, LZ_ASSIGN_ALWAYS);
    check_assign(&one_or_two, &floating, LZ_ASSIGN_NEVER);

    LzLiteral mixed_items[] = {string_literal("a"), int_literal(1)};
    LzType mixed = with_literals(0, mixed_items, 2);
    LzType string_or_int = kinds(LZ_KIND_STRING | LZ_KIND_INT);
    check_assign(&mixed, &string_or_int, LZ_ASSIGN_ALWAYS);
    check_assign(&string_or_int, &mixed, LZ_ASSIGN_MAYBE);

    LzLiteral true_item[] = {bool_literal(true)};
    LzType truthy = with_literals(0, true_item, 1);
    LzType boolean = kinds(LZ_KIND_BOOL);
    check_assign(&truthy, &boolean, LZ_ASSIGN_ALWAYS);
    check_assign(&boolean, &truthy, LZ_ASSIGN_MAYBE);
}

static void test_union_and_widen(void) {
    LzType integer = kinds(LZ_KIND_INT);
    LzType string = kinds(LZ_KIND_STRING);
    LzType joined;
    LZ_CHECK(lz_type_union(&joined, &integer, &string));
    LZ_CHECK(joined.kinds == (LZ_KIND_INT | LZ_KIND_STRING) && joined.literal_count == 0);

    LzType mode = modes();
    LzType both;
    LZ_CHECK(lz_type_union(&both, &mode, &string));
    LZ_CHECK(both.kinds == LZ_KIND_STRING && both.literal_count == 0);
    LZ_CHECK(lz_type_union(&both, &string, &mode));
    LZ_CHECK(both.kinds == LZ_KIND_STRING && both.literal_count == 0);

    LzLiteral extra[] = {string_literal("w"), string_literal("a")};
    LzType more = with_literals(0, extra, 2);
    LzType merged;
    LZ_CHECK(lz_type_union(&merged, &mode, &more));
    LZ_CHECK(merged.literal_count == 3);

    LzType numbered = numbers_12();
    LzType mixed;
    LZ_CHECK(lz_type_union(&mixed, &mode, &numbered));
    LzType widened = lz_type_widen(&mixed);
    LZ_CHECK(widened.kinds == (LZ_KIND_STRING | LZ_KIND_INT) && widened.literal_count == 0);
    LZ_CHECK(lz_type_possible_kinds(&mixed) == (LZ_KIND_STRING | LZ_KIND_INT));

    LzType eight = kinds(0);
    char name[2] = {'a', '\0'};
    for (int i = 0; i < LZ_TYPE_MAX_LITERALS; i++) {
        LzLiteral literal = string_literal(name);
        LZ_CHECK(lz_type_add_literal(&eight, &literal));
        name[0]++;
    }
    LzLiteral ninth = string_literal("z");
    LZ_CHECK(!lz_type_add_literal(&eight, &ninth));
    LzLiteral again = string_literal("c");
    LZ_CHECK(lz_type_add_literal(&eight, &again));

    LzType other = with_literals(0, (LzLiteral[]){string_literal("z")}, 1);
    LzType overflow;
    LZ_CHECK(!lz_type_union(&overflow, &eight, &other));
}

static void test_equality_of_types(void) {
    LzType a = modes();
    LzLiteral reversed[] = {string_literal("w"), string_literal("r")};
    LzType b = with_literals(0, reversed, 2);
    LZ_CHECK(lz_type_equal(&a, &b));

    LzLiteral other[] = {string_literal("r"), string_literal("x")};
    LzType c = with_literals(0, other, 2);
    LZ_CHECK(!lz_type_equal(&a, &c));
    LzType d = kinds(LZ_KIND_STRING);
    LZ_CHECK(!lz_type_equal(&a, &d));
}

static void test_descriptions(void) {
    LzType any = kinds(LZ_KINDS_ANY);
    LzType never = kinds(0);
    LzType number = kinds(LZ_KINDS_NUMBER);
    LZ_CHECK(described_as(&any, "any"));
    LZ_CHECK(described_as(&never, "never"));
    LZ_CHECK(described_as(&number, "number"));

    LzType string_or_int = kinds(LZ_KIND_STRING | LZ_KIND_INT);
    LzType nullable = kinds(LZ_KIND_STRING | LZ_KIND_NIL);
    LzType everything_but_nil = kinds(LZ_KINDS_ANY & ~LZ_KIND_NIL);
    LZ_CHECK(described_as(&string_or_int, "int | string"));
    LZ_CHECK(described_as(&nullable, "string | nil"));
    LZ_CHECK(described_as(&everything_but_nil, "number | string | bool | function | any[]"));

    LzType mode = modes();
    LZ_CHECK(described_as(&mode, "\"r\" | \"w\""));

    LzLiteral items[] = {int_literal(-3), bool_literal(false)};
    LzType literals = with_literals(LZ_KIND_STRING, items, 2);
    LZ_CHECK(described_as(&literals, "string | -3 | false"));

    LzLiteral long_item[] = {string_literal("abcdefghijklmnopqrstuvwxyz0123456789")};
    LzType long_text = with_literals(0, long_item, 1);
    LZ_CHECK(described_as(&long_text, "\"abcdefghijklmnopqrstuvwx...\""));
}

static void test_value_helpers(void) {
    LzValue five = lz_value_int(5);
    LzValue nil = lz_value_nil();
    LzValue half = lz_value_float(1.5);
    LzString *text = lz_string_new(&heap, "abc", 3);
    LzValue string = lz_value_object(&text->base);
    LZ_CHECK(value_described_as(&five, "5"));
    LZ_CHECK(value_described_as(&nil, "nil"));
    LZ_CHECK(value_described_as(&half, "1.5"));
    LZ_CHECK(value_described_as(&string, "\"abc\""));

    LzType of_five = lz_type_of_value(&five);
    LZ_CHECK(of_five.kinds == 0 && of_five.literal_count == 1);
    LzType of_half = lz_type_of_value(&half);
    LZ_CHECK(of_half.kinds == LZ_KIND_FLOAT && of_half.literal_count == 0);
    LzType of_nil = lz_type_of_value(&nil);
    LZ_CHECK(of_nil.kinds == LZ_KIND_NIL);
    LzType of_text = lz_type_of_value(&string);
    LZ_CHECK(of_text.literal_count == 1 && lz_type_possible_kinds(&of_text) == LZ_KIND_STRING);
    LzType integer = kinds(LZ_KIND_INT);
    check_assign(&of_five, &integer, LZ_ASSIGN_ALWAYS);
}

int main(void) {
    lz_heap_init(&heap);
    test_kinds();
    test_matching();
    test_assignability();
    test_union_and_widen();
    test_equality_of_types();
    test_descriptions();
    test_value_helpers();
    lz_heap_destroy(&heap);
    return LZ_TEST_RESULT();
}
