#include "lizard/lexer.h"

#include "lizard/number.h"

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
 * Escapes: \n \t \r \0 \\ \" \' \xHH (00 to 7F) and \u{H..H} (1 to 6 digits, a
 * scalar value). \x is limited to ASCII so a literal is valid UTF-8 whenever
 * the source is.
 */
static LzToken lex_string(LzLexer *lexer, size_t start) {
    const char *text = lexer->source->text;
    size_t length = lexer->source->length;
    char quote = text[start];
    size_t pos = start + 1;
    const char *error = NULL;

    for (;;) {
        if (pos >= length || text[pos] == '\n') {
            if (error == NULL) {
                error = "unterminated string";
            }
            break;
        }

        char c = text[pos];
        if (c == quote) {
            pos++;
            break;
        }
        if (c != '\\') {
            pos++;
            continue;
        }

        pos++;
        if (pos >= length || text[pos] == '\n') {
            if (error == NULL) {
                error = "unterminated string";
            }
            break;
        }

        switch (text[pos]) {
        case 'n':
        case 't':
        case 'r':
        case '0':
        case '\\':
        case '"':
        case '\'':
            pos++;
            break;
        case 'x': {
            pos++;
            int high = pos < length ? hex_value(text[pos]) : -1;
            int low = pos + 1 < length ? hex_value(text[pos + 1]) : -1;
            if (high < 0 || low < 0 || high > 7) {
                if (error == NULL) {
                    error = "invalid escape sequence";
                }
            } else {
                pos += 2;
            }
            break;
        }
        case 'u': {
            pos++;
            bool valid = false;
            if (pos < length && text[pos] == '{') {
                size_t probe = pos + 1;
                uint32_t scalar = 0;
                int digits = 0;
                while (probe < length && hex_value(text[probe]) >= 0 && digits < 7) {
                    scalar = scalar * 16 + (uint32_t)hex_value(text[probe]);
                    probe++;
                    digits++;
                }
                if (probe < length && text[probe] == '}' && digits >= 1 && digits <= 6 &&
                    scalar <= 0x10FFFF && !(scalar >= 0xD800 && scalar <= 0xDFFF)) {
                    valid = true;
                    pos = probe + 1;
                }
            }
            if (!valid && error == NULL) {
                error = "invalid unicode escape";
            }
            break;
        }
        default:
            if (error == NULL) {
                error = "invalid escape sequence";
            }
            pos++;
            while (pos < length && is_continuation(text[pos])) {
                pos++;
            }
            break;
        }
    }

    lexer->position = pos;
    if (error != NULL) {
        return make_error(lexer, start, error);
    }
    return make_token(lexer, LZ_TOKEN_STRING, start);
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
    if (is_alpha(c)) {
        return lex_identifier(lexer, start);
    }
    if (c == '"' || c == '\'') {
        return lex_string(lexer, start);
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
        return make_token(lexer, LZ_TOKEN_PIPE, start);
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
        return make_token(lexer, match(lexer, '=') ? LZ_TOKEN_LESS_EQUAL : LZ_TOKEN_LESS, start);
    case '>':
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

static bool write_utf8(const LzWriter *writer, uint32_t scalar) {
    char bytes[4];
    size_t count;
    if (scalar < 0x80) {
        bytes[0] = (char)scalar;
        count = 1;
    } else if (scalar < 0x800) {
        bytes[0] = (char)(0xC0 | (scalar >> 6));
        bytes[1] = (char)(0x80 | (scalar & 0x3F));
        count = 2;
    } else if (scalar < 0x10000) {
        bytes[0] = (char)(0xE0 | (scalar >> 12));
        bytes[1] = (char)(0x80 | ((scalar >> 6) & 0x3F));
        bytes[2] = (char)(0x80 | (scalar & 0x3F));
        count = 3;
    } else {
        bytes[0] = (char)(0xF0 | (scalar >> 18));
        bytes[1] = (char)(0x80 | ((scalar >> 12) & 0x3F));
        bytes[2] = (char)(0x80 | ((scalar >> 6) & 0x3F));
        bytes[3] = (char)(0x80 | (scalar & 0x3F));
        count = 4;
    }
    return lz_writer_write(writer, bytes, count);
}

bool lz_token_string_value(const LzSource *source, const LzToken *token, LzBuffer *out) {
    LzWriter writer = lz_writer_from_buffer(out);
    const char *text = source->text;
    size_t pos = (size_t)token->offset + 1;
    size_t end = (size_t)token->offset + token->length - 1;

    while (pos < end) {
        char c = text[pos++];
        if (c != '\\') {
            if (!lz_writer_write(&writer, &c, 1)) {
                return false;
            }
            continue;
        }

        char escape = text[pos++];
        char simple = '\0';
        switch (escape) {
        case 'n':
            simple = '\n';
            break;
        case 't':
            simple = '\t';
            break;
        case 'r':
            simple = '\r';
            break;
        case '0':
            simple = '\0';
            break;
        case '\\':
        case '"':
        case '\'':
            simple = escape;
            break;
        case 'x': {
            simple = (char)(hex_value(text[pos]) * 16 + hex_value(text[pos + 1]));
            pos += 2;
            break;
        }
        case 'u': {
            pos++;
            uint32_t scalar = 0;
            while (text[pos] != '}') {
                scalar = scalar * 16 + (uint32_t)hex_value(text[pos]);
                pos++;
            }
            pos++;
            if (!write_utf8(&writer, scalar)) {
                return false;
            }
            continue;
        }
        default:
            return false;
        }
        if (!lz_writer_write(&writer, &simple, 1)) {
            return false;
        }
    }
    return true;
}
