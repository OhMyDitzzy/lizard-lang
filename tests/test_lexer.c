#include <stdint.h>
#include <string.h>

#include "lizard/lexer.h"
#include "test_util.h"

#define T(name) LZ_TOKEN_##name
#define MAX_TOKENS 256

typedef struct Lexed {
    LzSource source;
    LzToken tokens[MAX_TOKENS];
    size_t count;
} Lexed;

static void lex_buffer(Lexed *out, const char *text, size_t length) {
    lz_source_from_buffer(&out->source, "t.lz", text, length);
    LzLexer lexer;
    lz_lexer_init(&lexer, &out->source);
    out->count = 0;
    for (;;) {
        LzToken token = lz_lexer_next(&lexer);
        if (out->count < MAX_TOKENS) {
            out->tokens[out->count++] = token;
        }
        if (token.type == LZ_TOKEN_EOF) {
            break;
        }
    }
}

static void lex(Lexed *out, const char *text) {
    lex_buffer(out, text, strlen(text));
}

static void check_types(const char *text, const LzTokenType *types, size_t count) {
    Lexed lexed;
    lex(&lexed, text);

    bool same = lexed.count == count + 1 && lexed.tokens[count].type == LZ_TOKEN_EOF;
    for (size_t i = 0; same && i < count; i++) {
        same = lexed.tokens[i].type == types[i];
    }
    if (!same) {
        fprintf(stderr, "token mismatch for \"%s\": got", text);
        for (size_t i = 0; i < lexed.count; i++) {
            fprintf(stderr, " %s", lz_token_type_name(lexed.tokens[i].type));
        }
        fprintf(stderr, "\n");
    }
    LZ_CHECK(same);
}

#define EXPECT(text, ...)                                                                          \
    do {                                                                                           \
        const LzTokenType expected_types[] = {__VA_ARGS__};                                        \
        check_types(text, expected_types, sizeof expected_types / sizeof expected_types[0]);       \
    } while (0)

static void check_empty(const char *text) {
    check_types(text, NULL, 0);
}

static void check_error(const char *text, const char *message) {
    Lexed lexed;
    lex(&lexed, text);
    LZ_CHECK(lexed.tokens[0].type == LZ_TOKEN_ERROR);
    if (lexed.tokens[0].type == LZ_TOKEN_ERROR) {
        LZ_CHECK(strstr(lexed.tokens[0].value.error, message) != NULL);
    }
}

static void check_single_error(const char *text, const char *message) {
    check_error(text, message);
    Lexed lexed;
    lex(&lexed, text);
    LZ_CHECK(lexed.tokens[0].offset == 0);
    LZ_CHECK(lexed.tokens[0].length == strlen(text));
}

static void check_integer(const char *text, int64_t expected) {
    Lexed lexed;
    lex(&lexed, text);
    LZ_CHECK(lexed.tokens[0].type == LZ_TOKEN_INTEGER);
    LZ_CHECK(lexed.tokens[0].value.integer == expected);
    LZ_CHECK(lexed.tokens[0].length == strlen(text));
}

static void check_float(const char *text, double expected) {
    Lexed lexed;
    lex(&lexed, text);
    LZ_CHECK(lexed.tokens[0].type == LZ_TOKEN_FLOAT);
    LZ_CHECK(lexed.tokens[0].value.number == expected);
    LZ_CHECK(lexed.tokens[0].length == strlen(text));
}

static void test_operators_and_punctuation(void) {
    EXPECT("+ - * / // % ( ) [ ] { } , . : ; = == != ! < <= > >=", T(PLUS), T(MINUS), T(STAR),
           T(SLASH), T(SLASH_SLASH), T(PERCENT), T(LEFT_PAREN), T(RIGHT_PAREN), T(LEFT_BRACKET),
           T(RIGHT_BRACKET), T(LEFT_BRACE), T(RIGHT_BRACE), T(COMMA), T(DOT), T(COLON),
           T(SEMICOLON), T(EQUAL), T(EQUAL_EQUAL), T(BANG_EQUAL), T(BANG), T(LESS), T(LESS_EQUAL),
           T(GREATER), T(GREATER_EQUAL), T(NEWLINE));
    EXPECT("a///b", T(IDENTIFIER), T(SLASH_SLASH), T(SLASH), T(IDENTIFIER), T(NEWLINE));
    EXPECT("!==", T(BANG_EQUAL), T(EQUAL), T(NEWLINE));
    EXPECT("===", T(EQUAL_EQUAL), T(EQUAL), T(NEWLINE));
}

static void test_keywords_and_identifiers(void) {
    EXPECT("and or true false nil android orx nile _a a1 A_b", T(AND), T(OR), T(TRUE), T(FALSE),
           T(NIL), T(IDENTIFIER), T(IDENTIFIER), T(IDENTIFIER), T(IDENTIFIER), T(IDENTIFIER),
           T(IDENTIFIER), T(NEWLINE));
    EXPECT("And NIL", T(IDENTIFIER), T(IDENTIFIER), T(NEWLINE));
    EXPECT("let const if else while letx iff whiled", T(LET), T(CONST), T(IF), T(ELSE), T(WHILE),
           T(IDENTIFIER), T(IDENTIFIER), T(IDENTIFIER), T(NEWLINE));
}

static void test_integers(void) {
    check_integer("0", 0);
    check_integer("42", 42);
    check_integer("007", 7);
    check_integer("1_000", 1000);
    check_integer("0xFF", 255);
    check_integer("0Xab_cd", 0xABCD);
    check_integer("0b101", 5);
    check_integer("0o17", 15);
    check_integer("9223372036854775807", INT64_MAX);
    check_integer("0x7FFFFFFFFFFFFFFF", INT64_MAX);
    /* 2^63 only exists so that -9223372036854775808 can be written: it comes back as INT64_MIN. */
    check_integer("9223372036854775808", INT64_MIN);
    check_integer("0x8000000000000000", INT64_MIN);
    EXPECT("-9223372036854775808", T(MINUS), T(INTEGER), T(NEWLINE));
    check_error("9223372036854775809", "out of range");
    check_error("0x8000000000000001", "out of range");
    check_error("0xFFFFFFFFFFFFFFFF", "out of range");
    check_error("99999999999999999999999", "out of range");
}

static void test_floats(void) {
    check_float("1.5", 1.5);
    check_float("0.25", 0.25);
    check_float("1e3", 1000.0);
    check_float("1.5e-2", 0.015);
    check_float("2E+2", 200.0);
    check_float("1_0.2_5", 10.25);
    check_float("1e0", 1.0);
    check_float("1e-999", 0.0);
    check_error("1e999", "out of range");
    EXPECT("1.foo", T(INTEGER), T(DOT), T(IDENTIFIER), T(NEWLINE));
    EXPECT("1..2", T(INTEGER), T(DOT), T(DOT), T(INTEGER), T(NEWLINE));
    EXPECT("1.5.2", T(FLOAT), T(DOT), T(INTEGER), T(NEWLINE));
    EXPECT("1._5", T(INTEGER), T(DOT), T(IDENTIFIER), T(NEWLINE));
    EXPECT(".5", T(DOT), T(INTEGER), T(NEWLINE));
}

static void test_malformed_numbers(void) {
    const char *const bad[] = {"1__0", "1_",  "0x",   "0xZ",  "0b2", "1e",    "1e+",
                               "12abc", "1.5x", "0b1e5", "1_.5", "0x_1", "1e_5"};
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        check_single_error(bad[i], "malformed number");
        EXPECT(bad[i], T(ERROR), T(NEWLINE));
    }
}

static void test_strings(void) {
    EXPECT("\"hi\" 'yo' \"\"", T(STRING), T(STRING), T(STRING), T(NEWLINE));
    EXPECT("\"a'b\" 'a\"b'", T(STRING), T(STRING), T(NEWLINE));

    Lexed lexed;
    LzBuffer value = {0};

    lex(&lexed, "\"a\\n\\t\\\\\\\"b\"");
    LZ_CHECK(lexed.tokens[0].type == LZ_TOKEN_STRING);
    LZ_CHECK(lz_token_string_value(&lexed.source, &lexed.tokens[0], &value));
    LZ_CHECK(value.length == 6 && memcmp(value.data, "a\n\t\\\"b", 6) == 0);
    lz_buffer_free(&value);

    lex(&lexed, "'\\x41\\u{1F600}\\u{e9}\\0'");
    LZ_CHECK(lexed.tokens[0].type == LZ_TOKEN_STRING);
    LZ_CHECK(lz_token_string_value(&lexed.source, &lexed.tokens[0], &value));
    LZ_CHECK(value.length == 1 + 4 + 2 + 1);
    LZ_CHECK(memcmp(value.data, "A\xF0\x9F\x98\x80\xC3\xA9", 7) == 0);
    LZ_CHECK(value.data[7] == '\0');
    lz_buffer_free(&value);

    lex(&lexed, "\"\"");
    LZ_CHECK(lz_token_string_value(&lexed.source, &lexed.tokens[0], &value));
    LZ_CHECK(value.length == 0);
    lz_buffer_free(&value);

    lex(&lexed, "\"caf\xC3\xA9\"");
    LZ_CHECK(lz_token_string_value(&lexed.source, &lexed.tokens[0], &value));
    LZ_CHECK(value.length == 5 && memcmp(value.data, "caf\xC3\xA9", 5) == 0);
    lz_buffer_free(&value);
}

static void test_string_errors(void) {
    check_error("\"abc", "unterminated string");
    check_error("\"abc\n\"", "unterminated string");
    check_error("'abc\\", "unterminated string");
    check_error("\"\\q\"", "invalid escape sequence");
    check_error("\"\\x8f\"", "invalid escape sequence");
    check_error("\"\\xZ1\"", "invalid escape sequence");
    check_error("\"\\x4\"", "invalid escape sequence");
    check_error("\"\\u{110000}\"", "invalid unicode escape");
    check_error("\"\\u{D800}\"", "invalid unicode escape");
    check_error("\"\\u{}\"", "invalid unicode escape");
    check_error("\"\\u41\"", "invalid unicode escape");
    check_error("\"\\u{1234567}\"", "invalid unicode escape");

    EXPECT("\"abc\nx", T(ERROR), T(NEWLINE), T(IDENTIFIER), T(NEWLINE));
    EXPECT("\"\\q\" x", T(ERROR), T(IDENTIFIER), T(NEWLINE));
}

static void test_comments_and_newlines(void) {
    EXPECT("a # comment\n\n\nb\n", T(IDENTIFIER), T(NEWLINE), T(IDENTIFIER), T(NEWLINE));
    EXPECT("\n\n a", T(IDENTIFIER), T(NEWLINE));
    EXPECT("a", T(IDENTIFIER), T(NEWLINE));
    check_empty("");
    check_empty("# only a comment\n");
    check_empty("# no trailing newline");
    check_empty("   \n  \t \n");
    EXPECT("a\r\nb\r\n", T(IDENTIFIER), T(NEWLINE), T(IDENTIFIER), T(NEWLINE));
    EXPECT("a # c", T(IDENTIFIER), T(NEWLINE));
    EXPECT("a #\"unterminated\n b", T(IDENTIFIER), T(NEWLINE), T(IDENTIFIER), T(NEWLINE));

    Lexed lexed;
    lex(&lexed, "ab\ncd");
    LZ_CHECK(lexed.tokens[1].type == LZ_TOKEN_NEWLINE);
    LZ_CHECK(lexed.tokens[1].offset == 2 && lexed.tokens[1].length == 1);
    LZ_CHECK(lexed.tokens[3].type == LZ_TOKEN_NEWLINE);
    LZ_CHECK(lexed.tokens[3].offset == 5 && lexed.tokens[3].length == 0);
}

static void test_implicit_line_joining(void) {
    EXPECT("f(1,\n 2)\n", T(IDENTIFIER), T(LEFT_PAREN), T(INTEGER), T(COMMA), T(INTEGER),
           T(RIGHT_PAREN), T(NEWLINE));
    EXPECT("[1,\n2]", T(LEFT_BRACKET), T(INTEGER), T(COMMA), T(INTEGER), T(RIGHT_BRACKET),
           T(NEWLINE));
    EXPECT("f(g(\n1\n),\n2)", T(IDENTIFIER), T(LEFT_PAREN), T(IDENTIFIER), T(LEFT_PAREN),
           T(INTEGER), T(RIGHT_PAREN), T(COMMA), T(INTEGER), T(RIGHT_PAREN), T(NEWLINE));
    EXPECT("{\n}", T(LEFT_BRACE), T(NEWLINE), T(RIGHT_BRACE), T(NEWLINE));
    EXPECT(")\n", T(RIGHT_PAREN), T(NEWLINE));
    EXPECT("(\n", T(LEFT_PAREN));
    EXPECT("(1,\n# c\n2)\nx", T(LEFT_PAREN), T(INTEGER), T(COMMA), T(INTEGER), T(RIGHT_PAREN),
           T(NEWLINE), T(IDENTIFIER), T(NEWLINE));
}

static void test_offsets_and_lengths(void) {
    Lexed lexed;
    lex(&lexed, "ab + 12");
    LZ_CHECK(lexed.tokens[0].offset == 0 && lexed.tokens[0].length == 2);
    LZ_CHECK(lexed.tokens[1].offset == 3 && lexed.tokens[1].length == 1);
    LZ_CHECK(lexed.tokens[2].offset == 5 && lexed.tokens[2].length == 2);

    lex(&lexed, "x == y");
    LZ_CHECK(lexed.tokens[1].type == LZ_TOKEN_EQUAL_EQUAL);
    LZ_CHECK(lexed.tokens[1].offset == 2 && lexed.tokens[1].length == 2);

    lex(&lexed, "\"hi\"");
    LZ_CHECK(lexed.tokens[0].offset == 0 && lexed.tokens[0].length == 4);
}

static void test_unexpected_characters(void) {
    EXPECT("a @ b", T(IDENTIFIER), T(ERROR), T(IDENTIFIER), T(NEWLINE));
    EXPECT("$ `", T(ERROR), T(ERROR), T(NEWLINE));
    check_error("@", "unexpected character");

    Lexed lexed;
    lex(&lexed, "\xC3\xA9x");
    LZ_CHECK(lexed.tokens[0].type == LZ_TOKEN_ERROR);
    LZ_CHECK(lexed.tokens[0].offset == 0 && lexed.tokens[0].length == 2);
    LZ_CHECK(lexed.tokens[1].type == LZ_TOKEN_IDENTIFIER);

    lex(&lexed, "\xF0\x9F\x98\x80");
    LZ_CHECK(lexed.tokens[0].type == LZ_TOKEN_ERROR && lexed.tokens[0].length == 4);

    lex_buffer(&lexed, "a\0b", 3);
    LZ_CHECK(lexed.tokens[0].type == LZ_TOKEN_IDENTIFIER);
    LZ_CHECK(lexed.tokens[1].type == LZ_TOKEN_ERROR);
    LZ_CHECK(lexed.tokens[2].type == LZ_TOKEN_IDENTIFIER);
}

static void test_eof_is_sticky(void) {
    LzSource source;
    lz_source_from_string(&source, "t.lz", "x");
    LzLexer lexer;
    lz_lexer_init(&lexer, &source);
    LZ_CHECK(lz_lexer_next(&lexer).type == LZ_TOKEN_IDENTIFIER);
    LZ_CHECK(lz_lexer_next(&lexer).type == LZ_TOKEN_NEWLINE);
    for (int i = 0; i < 3; i++) {
        LzToken token = lz_lexer_next(&lexer);
        LZ_CHECK(token.type == LZ_TOKEN_EOF && token.offset == 1);
    }
}

static void test_a_realistic_line(void) {
    EXPECT("println(\"sum:\", 1 + 2 * 3.5) # done\nx = !y and z != nil\n", T(IDENTIFIER),
           T(LEFT_PAREN), T(STRING), T(COMMA), T(INTEGER), T(PLUS), T(INTEGER), T(STAR), T(FLOAT),
           T(RIGHT_PAREN), T(NEWLINE), T(IDENTIFIER), T(EQUAL), T(BANG), T(IDENTIFIER), T(AND),
           T(IDENTIFIER), T(BANG_EQUAL), T(NIL), T(NEWLINE));
}

static void test_type_names(void) {
    LZ_CHECK(strcmp(lz_token_type_name(LZ_TOKEN_PLUS), "PLUS") == 0);
    LZ_CHECK(strcmp(lz_token_type_name(LZ_TOKEN_SLASH_SLASH), "SLASH_SLASH") == 0);
    LZ_CHECK(strcmp(lz_token_type_name(LZ_TOKEN_EOF), "EOF") == 0);
}

int main(void) {
    test_operators_and_punctuation();
    test_keywords_and_identifiers();
    test_integers();
    test_floats();
    test_malformed_numbers();
    test_strings();
    test_string_errors();
    test_comments_and_newlines();
    test_implicit_line_joining();
    test_offsets_and_lengths();
    test_unexpected_characters();
    test_eof_is_sticky();
    test_a_realistic_line();
    test_type_names();
    return LZ_TEST_RESULT();
}
