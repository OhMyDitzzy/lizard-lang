#include "lizard/lexer.h"

#include "lizard/number.h"
#include "lizard/strlit.h"

#include <errno.h>
#include <locale.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct Keyword {
    const char *text;
    LzTokenType type;
} Keyword;

static const Keyword keywords[] = {
    {"and", LZ_TOKEN_AND},   {"or", LZ_TOKEN_OR},   {"true", LZ_TOKEN_TRUE},
    {"false", LZ_TOKEN_FALSE}, {"nil", LZ_TOKEN_NIL}, {"let", LZ_TOKEN_LET},
    {"const", LZ_TOKEN_CONST}, {"if", LZ_TOKEN_IF},   {"else", LZ_TOKEN_ELSE},
    {"while", LZ_TOKEN_WHILE}, {"fn", LZ_TOKEN_FN},   {"return", LZ_TOKEN_RETURN},
    /* Words for symbols: `not x` is `!x`, `a equal b` is `a == b`, `a not equal b` is `a != b`. */
    {"not", LZ_TOKEN_BANG},  {"equal", LZ_TOKEN_EQUAL_EQUAL},
};

/*
 * An integer literal is a magnitude; the sign is a unary minus. The largest valid
 * magnitude is 2^63 so that -9223372036854775808 can be written. A literal of
 * exactly 2^63 is returned as INT64_MIN and the compiler accepts it only right
 * after a unary minus.
 */
#define INTEGER_LIMIT ((uint64_t)INT64_MAX + 1u)

static int64_t integer_from_magnitude(uint64_t magnitude) {
    return magnitude == INTEGER_LIMIT ? INT64_MIN : (int64_t)magnitude;
}

static bool is_digit(char c) {
    return c >= '0' && c <= '9';
}

static bool is_alpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static bool is_alnum(char c) {
    return is_alpha(c) || is_digit(c);
}

static int hex_value(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

static bool is_continuation(char c) {
    return ((unsigned char)c & 0xC0) == 0x80;
}

void lz_lexer_init(LzLexer *lexer, const LzSource *source) {
    lexer->source = source;
    lexer->position = 0;
    lexer->nesting = 0;
    lexer->after_newline = true;
}

static LzToken make_token(const LzLexer *lexer, LzTokenType type, size_t start) {
    LzToken token;
    memset(&token, 0, sizeof token);
    token.type = type;
    token.offset = (uint32_t)start;
    token.length = (uint32_t)(lexer->position - start);
    return token;
}

static LzToken make_error(const LzLexer *lexer, size_t start, const char *message) {
    LzToken token = make_token(lexer, LZ_TOKEN_ERROR, start);
    token.value.error = message;
    return token;
}

static bool match(LzLexer *lexer, char expected) {
    if (lexer->position < lexer->source->length &&
        lexer->source->text[lexer->position] == expected) {
        lexer->position++;
        return true;
    }
    return false;
}

static LzToken lex_identifier(LzLexer *lexer, size_t start) {
    const char *text = lexer->source->text;
    while (lexer->position < lexer->source->length && is_alnum(text[lexer->position])) {
        lexer->position++;
    }

    size_t length = lexer->position - start;
    if (length == 3 && memcmp(text + start, "not", 3) == 0) {
        /* `not equal` is one token, `!=`. */
        size_t probe = lexer->position;
        while (probe < lexer->source->length && (text[probe] == ' ' || text[probe] == '\t')) {
            probe++;
        }
        if (probe > lexer->position && probe + 5 <= lexer->source->length &&
            memcmp(text + probe, "equal", 5) == 0 &&
            (probe + 5 == lexer->source->length || !is_alnum(text[probe + 5]))) {
            lexer->position = probe + 5;
            return make_token(lexer, LZ_TOKEN_BANG_EQUAL, start);
        }
    }
    for (size_t i = 0; i < sizeof keywords / sizeof keywords[0]; i++) {
        if (strlen(keywords[i].text) == length && memcmp(keywords[i].text, text + start, length) == 0) {
            return make_token(lexer, keywords[i].type, start);
        }
    }
    return make_token(lexer, LZ_TOKEN_IDENTIFIER, start);
}

/* A run of digits and underscores: no leading, trailing or doubled underscore. */
static bool digit_run_ok(const char *text, size_t length) {
    if (length == 0 || text[0] == '_' || text[length - 1] == '_') {
        return false;
    }
    for (size_t i = 0; i + 1 < length; i++) {
        if (text[i] == '_' && text[i + 1] == '_') {
            return false;
        }
    }
    return true;
}

typedef enum FloatResult {
    FLOAT_OK,
    FLOAT_RANGE,
    FLOAT_NO_MEMORY
} FloatResult;

/* The lexeme may contain digit separators; the shared parser wants plain digits. */
static FloatResult parse_float(const char *text, size_t length, double *out) {
    char *buffer = malloc(length + 1);
    if (buffer == NULL) {
        return FLOAT_NO_MEMORY;
    }
    size_t used = 0;
    for (size_t i = 0; i < length; i++) {
        if (text[i] != '_') {
            buffer[used++] = text[i];
        }
    }

    LzParseResult result = lz_parse_float(buffer, used, out);
    free(buffer);
    switch (result) {
    case LZ_PARSE_OK:
        return FLOAT_OK;
    case LZ_PARSE_NO_MEMORY:
        return FLOAT_NO_MEMORY;
    default:
        return FLOAT_RANGE;
    }
}

static LzToken lex_prefixed_integer(LzLexer *lexer, size_t start, unsigned base) {
    const char *text = lexer->source->text;
    size_t length = lexer->source->length;

    size_t pos = start + 2;
    size_t digits = pos;
    while (pos < length && is_alnum(text[pos])) {
        pos++;
    }
    lexer->position = pos;

    if (!digit_run_ok(text + digits, pos - digits)) {
        return make_error(lexer, start, "malformed number");
    }
    for (size_t i = digits; i < pos; i++) {
        if (text[i] != '_' && (hex_value(text[i]) < 0 || (unsigned)hex_value(text[i]) >= base)) {
            return make_error(lexer, start, "malformed number");
        }
    }

    uint64_t value = 0;
    for (size_t i = digits; i < pos; i++) {
        if (text[i] == '_') {
            continue;
        }
        uint64_t digit = (uint64_t)hex_value(text[i]);
        if (value > (INTEGER_LIMIT - digit) / base) {
            return make_error(lexer, start, "integer literal out of range");
        }
        value = value * base + digit;
    }

    LzToken token = make_token(lexer, LZ_TOKEN_INTEGER, start);
    token.value.integer = integer_from_magnitude(value);
    return token;
}

static LzToken lex_number(LzLexer *lexer, size_t start) {
    const char *text = lexer->source->text;
    size_t length = lexer->source->length;

    if (text[start] == '0' && start + 1 < length) {
        char prefix = text[start + 1];
        if (prefix == 'x' || prefix == 'X') {
            return lex_prefixed_integer(lexer, start, 16);
        }
        if (prefix == 'b' || prefix == 'B') {
            return lex_prefixed_integer(lexer, start, 2);
        }
        if (prefix == 'o' || prefix == 'O') {
            return lex_prefixed_integer(lexer, start, 8);
        }
    }

    size_t pos = start;
    while (pos < length && (is_digit(text[pos]) || text[pos] == '_')) {
        pos++;
    }
    bool ok = digit_run_ok(text + start, pos - start);
    bool is_float = false;

    if (pos + 1 < length && text[pos] == '.' && is_digit(text[pos + 1])) {
        is_float = true;
        pos++;
        size_t fraction = pos;
        while (pos < length && (is_digit(text[pos]) || text[pos] == '_')) {
            pos++;
        }
        ok = ok && digit_run_ok(text + fraction, pos - fraction);
    }

    if (pos < length && (text[pos] == 'e' || text[pos] == 'E')) {
        size_t probe = pos + 1;
        if (probe < length && (text[probe] == '+' || text[probe] == '-')) {
            probe++;
        }
        if (probe < length && is_digit(text[probe])) {
            is_float = true;
            size_t exponent = probe;
            while (probe < length && (is_digit(text[probe]) || text[probe] == '_')) {
                probe++;
            }
            ok = ok && digit_run_ok(text + exponent, probe - exponent);
        } else {
            ok = false;
        }
        pos = probe;
    }

    if (pos < length && is_alnum(text[pos])) {
        ok = false;
        while (pos < length && is_alnum(text[pos])) {
            pos++;
        }
    }

    lexer->position = pos;
    if (!ok) {
        return make_error(lexer, start, "malformed number");
    }

    if (is_float) {
        double value = 0;
        FloatResult result = parse_float(text + start, pos - start, &value);
        if (result == FLOAT_NO_MEMORY) {
            return make_error(lexer, start, "out of memory");
        }
        if (result == FLOAT_RANGE) {
            return make_error(lexer, start, "float literal out of range");
        }
        LzToken token = make_token(lexer, LZ_TOKEN_FLOAT, start);
        token.value.number = value;
        return token;
    }

    uint64_t value = 0;
    for (size_t i = start; i < pos; i++) {
        if (text[i] == '_') {
            continue;
        }
        uint64_t digit = (uint64_t)(text[i] - '0');
        if (value > (INTEGER_LIMIT - digit) / 10) {
            return make_error(lexer, start, "integer literal out of range");
        }
        value = value * 10 + digit;
    }
    LzToken token = make_token(lexer, LZ_TOKEN_INTEGER, start);
    token.value.integer = integer_from_magnitude(value);
    return token;
}

/*
 * A string literal of any kind (see strlit.h). The scan finds its end; a second walk with
 * the reader reports escapes, braces and indentation that are wrong, so the compiler can
 * decode a STRING token without failing.
 */
static LzToken lex_string(LzLexer *lexer, size_t start) {
    LzStringHead head;
    LzStringError error;
    if (!lz_string_scan(lexer->source, start, &head, &error)) {
        lexer->position = error.resume;
        LzToken token = make_error(lexer, start, error.message);
        token.offset = (uint32_t)error.offset;
        token.length = (uint32_t)error.length;
        return token;
    }

    if (head.needs_check) {
        LzStringReader reader;
        lz_string_reader_start(&reader, lexer->source, &head);
        LzBuffer scratch = {0};
        for (;;) {
            LzStringPart part;
            scratch.length = 0;
            if (!lz_string_reader_next(&reader, &scratch, &part)) {
                lz_buffer_free(&scratch);
                lexer->position = head.end;
                return make_error(lexer, start, "out of memory");
            }
            if (part.kind == LZ_STRING_ERROR) {
                lz_buffer_free(&scratch);
                lexer->position = head.end;
                LzToken token = make_error(lexer, start, part.message);
                token.offset = (uint32_t)part.offset;
                token.length = (uint32_t)part.length;
                return token;
            }
            if (part.kind == LZ_STRING_END) {
                break;
            }
        }
        lz_buffer_free(&scratch);
    }

    lexer->position = head.end;
    LzToken token = make_token(lexer, LZ_TOKEN_STRING, start);
    token.value.string.is_template = head.is_template;
    token.value.string.is_raw = head.is_raw;
    token.value.string.is_multiline = head.is_multiline;
    return token;
}

LzToken lz_lexer_next(LzLexer *lexer) {
    const char *text = lexer->source->text;
    size_t length = lexer->source->length;

    for (;;) {
        while (lexer->position < length &&
               (text[lexer->position] == ' ' || text[lexer->position] == '\t' ||
                text[lexer->position] == '\r')) {
            lexer->position++;
        }

        if (lexer->position >= length) {
            if (!lexer->after_newline && lexer->nesting == 0) {
                lexer->after_newline = true;
                return make_token(lexer, LZ_TOKEN_NEWLINE, length);
            }
            return make_token(lexer, LZ_TOKEN_EOF, length);
        }

        char c = text[lexer->position];
        if (c == '#') {
            while (lexer->position < length && text[lexer->position] != '\n') {
                lexer->position++;
            }
            continue;
        }
        if (c == '\n') {
            size_t at = lexer->position++;
            if (lexer->nesting > 0 || lexer->after_newline) {
                continue;
            }
            lexer->after_newline = true;
            return make_token(lexer, LZ_TOKEN_NEWLINE, at);
        }
        break;
    }

    size_t start = lexer->position;
    char c = text[start];
    lexer->after_newline = false;

    if (is_digit(c)) {
        return lex_number(lexer, start);
    }
    if (lz_string_starts_at(lexer->source, start)) {
        return lex_string(lexer, start);
    }
    if (is_alpha(c)) {
        return lex_identifier(lexer, start);
    }

    lexer->position++;
    switch (c) {
    case '(':
        lexer->nesting++;
        return make_token(lexer, LZ_TOKEN_LEFT_PAREN, start);
    case ')':
        if (lexer->nesting > 0) {
            lexer->nesting--;
        }
        return make_token(lexer, LZ_TOKEN_RIGHT_PAREN, start);
    case '[':
        lexer->nesting++;
        return make_token(lexer, LZ_TOKEN_LEFT_BRACKET, start);
    case ']':
        if (lexer->nesting > 0) {
            lexer->nesting--;
        }
        return make_token(lexer, LZ_TOKEN_RIGHT_BRACKET, start);
    case '{':
        return make_token(lexer, LZ_TOKEN_LEFT_BRACE, start);
    case '}':
        return make_token(lexer, LZ_TOKEN_RIGHT_BRACE, start);
    case ',':
        return make_token(lexer, LZ_TOKEN_COMMA, start);
    case '.':
        return make_token(lexer, LZ_TOKEN_DOT, start);
    case ':':
        return make_token(lexer, LZ_TOKEN_COLON, start);
    case ';':
        return make_token(lexer, LZ_TOKEN_SEMICOLON, start);
    case '+':
        return make_token(lexer, LZ_TOKEN_PLUS, start);
    case '-':
        return make_token(lexer, match(lexer, '>') ? LZ_TOKEN_ARROW : LZ_TOKEN_MINUS, start);
    case '*':
        return make_token(lexer, LZ_TOKEN_STAR, start);
    case '%':
        return make_token(lexer, LZ_TOKEN_PERCENT, start);
    case '|':
        return make_token(lexer, match(lexer, '|') ? LZ_TOKEN_OR : LZ_TOKEN_PIPE, start);
    case '&':
        return make_token(lexer, match(lexer, '&') ? LZ_TOKEN_AND : LZ_TOKEN_AMPERSAND, start);
    case '^':
        return make_token(lexer, LZ_TOKEN_CARET, start);
    case '~':
        return make_token(lexer, LZ_TOKEN_TILDE, start);
    case '?':
        return make_token(lexer, LZ_TOKEN_QUESTION, start);
    case '/':
        return make_token(lexer, match(lexer, '/') ? LZ_TOKEN_SLASH_SLASH : LZ_TOKEN_SLASH, start);
    case '!':
        return make_token(lexer, match(lexer, '=') ? LZ_TOKEN_BANG_EQUAL : LZ_TOKEN_BANG, start);
    case '=':
        if (match(lexer, '>')) {
            return make_token(lexer, LZ_TOKEN_FAT_ARROW, start);
        }
        return make_token(lexer, match(lexer, '=') ? LZ_TOKEN_EQUAL_EQUAL : LZ_TOKEN_EQUAL, start);
    case '<':
        if (match(lexer, '<')) {
            return make_token(lexer, LZ_TOKEN_LESS_LESS, start);
        }
        return make_token(lexer, match(lexer, '=') ? LZ_TOKEN_LESS_EQUAL : LZ_TOKEN_LESS, start);
    case '>':
        if (match(lexer, '>')) {
            return make_token(lexer, LZ_TOKEN_GREATER_GREATER, start);
        }
        return make_token(lexer, match(lexer, '=') ? LZ_TOKEN_GREATER_EQUAL : LZ_TOKEN_GREATER,
                          start);
    default:
        while (lexer->position < length && is_continuation(text[lexer->position])) {
            lexer->position++;
        }
        return make_error(lexer, start, "unexpected character");
    }
}

const char *lz_token_type_name(LzTokenType type) {
    switch (type) {
    case LZ_TOKEN_EOF:
        return "EOF";
    case LZ_TOKEN_ERROR:
        return "ERROR";
    case LZ_TOKEN_NEWLINE:
        return "NEWLINE";
    case LZ_TOKEN_IDENTIFIER:
        return "IDENTIFIER";
    case LZ_TOKEN_INTEGER:
        return "INTEGER";
    case LZ_TOKEN_FLOAT:
        return "FLOAT";
    case LZ_TOKEN_STRING:
        return "STRING";
    case LZ_TOKEN_LEFT_PAREN:
        return "LEFT_PAREN";
    case LZ_TOKEN_RIGHT_PAREN:
        return "RIGHT_PAREN";
    case LZ_TOKEN_LEFT_BRACKET:
        return "LEFT_BRACKET";
    case LZ_TOKEN_RIGHT_BRACKET:
        return "RIGHT_BRACKET";
    case LZ_TOKEN_LEFT_BRACE:
        return "LEFT_BRACE";
    case LZ_TOKEN_RIGHT_BRACE:
        return "RIGHT_BRACE";
    case LZ_TOKEN_COMMA:
        return "COMMA";
    case LZ_TOKEN_DOT:
        return "DOT";
    case LZ_TOKEN_COLON:
        return "COLON";
    case LZ_TOKEN_SEMICOLON:
        return "SEMICOLON";
    case LZ_TOKEN_PLUS:
        return "PLUS";
    case LZ_TOKEN_MINUS:
        return "MINUS";
    case LZ_TOKEN_STAR:
        return "STAR";
    case LZ_TOKEN_SLASH:
        return "SLASH";
    case LZ_TOKEN_SLASH_SLASH:
        return "SLASH_SLASH";
    case LZ_TOKEN_PERCENT:
        return "PERCENT";
    case LZ_TOKEN_PIPE:
        return "PIPE";
    case LZ_TOKEN_AMPERSAND:
        return "AMPERSAND";
    case LZ_TOKEN_CARET:
        return "CARET";
    case LZ_TOKEN_TILDE:
        return "TILDE";
    case LZ_TOKEN_LESS_LESS:
        return "LESS_LESS";
    case LZ_TOKEN_GREATER_GREATER:
        return "GREATER_GREATER";
    case LZ_TOKEN_QUESTION:
        return "QUESTION";
    case LZ_TOKEN_BANG:
        return "BANG";
    case LZ_TOKEN_BANG_EQUAL:
        return "BANG_EQUAL";
    case LZ_TOKEN_EQUAL:
        return "EQUAL";
    case LZ_TOKEN_EQUAL_EQUAL:
        return "EQUAL_EQUAL";
    case LZ_TOKEN_LESS:
        return "LESS";
    case LZ_TOKEN_LESS_EQUAL:
        return "LESS_EQUAL";
    case LZ_TOKEN_GREATER:
        return "GREATER";
    case LZ_TOKEN_GREATER_EQUAL:
        return "GREATER_EQUAL";
    case LZ_TOKEN_AND:
        return "AND";
    case LZ_TOKEN_OR:
        return "OR";
    case LZ_TOKEN_TRUE:
        return "TRUE";
    case LZ_TOKEN_FALSE:
        return "FALSE";
    case LZ_TOKEN_NIL:
        return "NIL";
    case LZ_TOKEN_LET:
        return "LET";
    case LZ_TOKEN_CONST:
        return "CONST";
    case LZ_TOKEN_IF:
        return "IF";
    case LZ_TOKEN_ELSE:
        return "ELSE";
    case LZ_TOKEN_WHILE:
        return "WHILE";
    case LZ_TOKEN_FN:
        return "FN";
    case LZ_TOKEN_RETURN:
        return "RETURN";
    case LZ_TOKEN_ARROW:
        return "ARROW";
    case LZ_TOKEN_FAT_ARROW:
        return "FAT_ARROW";
    }
    return "UNKNOWN";
}

bool lz_token_string_value(const LzSource *source, const LzToken *token, LzBuffer *out) {
    if (token->value.string.is_template) {
        return false;
    }
    LzStringReader reader;
    if (!lz_string_reader_init(&reader, source, token->offset)) {
        return false;
    }
    for (;;) {
        LzStringPart part;
        if (!lz_string_reader_next(&reader, out, &part)) {
            return false;
        }
        if (part.kind == LZ_STRING_END) {
            return true;
        }
        if (part.kind != LZ_STRING_TEXT) {
            return false;
        }
    }
}
