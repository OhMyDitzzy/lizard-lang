#ifndef LIZARD_LEXER_H
#define LIZARD_LEXER_H

#include "lizard/source.h"

typedef enum LzTokenType {
    LZ_TOKEN_EOF,
    LZ_TOKEN_ERROR,
    LZ_TOKEN_NEWLINE,
    LZ_TOKEN_IDENTIFIER,
    LZ_TOKEN_INTEGER,
    LZ_TOKEN_FLOAT,
    LZ_TOKEN_STRING,
    LZ_TOKEN_LEFT_PAREN,
    LZ_TOKEN_RIGHT_PAREN,
    LZ_TOKEN_LEFT_BRACKET,
    LZ_TOKEN_RIGHT_BRACKET,
    LZ_TOKEN_LEFT_BRACE,
    LZ_TOKEN_RIGHT_BRACE,
    LZ_TOKEN_COMMA,
    LZ_TOKEN_DOT,
    LZ_TOKEN_COLON,
    LZ_TOKEN_SEMICOLON,
    LZ_TOKEN_PLUS,
    LZ_TOKEN_MINUS,
    LZ_TOKEN_STAR,
    LZ_TOKEN_SLASH,
    LZ_TOKEN_SLASH_SLASH,
    LZ_TOKEN_PERCENT,
    LZ_TOKEN_PIPE,
    LZ_TOKEN_QUESTION,
    LZ_TOKEN_BANG,
    LZ_TOKEN_BANG_EQUAL,
    LZ_TOKEN_EQUAL,
    LZ_TOKEN_EQUAL_EQUAL,
    LZ_TOKEN_LESS,
    LZ_TOKEN_LESS_EQUAL,
    LZ_TOKEN_GREATER,
    LZ_TOKEN_GREATER_EQUAL,
    LZ_TOKEN_AND,
    LZ_TOKEN_OR,
    LZ_TOKEN_TRUE,
    LZ_TOKEN_FALSE,
    LZ_TOKEN_NIL,
    LZ_TOKEN_LET,
    LZ_TOKEN_CONST,
    LZ_TOKEN_IF,
    LZ_TOKEN_ELSE,
    LZ_TOKEN_WHILE
} LzTokenType;

/*
 * A token is a span of the source: [offset, offset + length). The lexeme is
 * never copied. INTEGER and FLOAT carry their value (an integer literal is a
 * magnitude, and 2^63 is returned as INT64_MIN: valid only after a unary minus);
 * ERROR carries a static message. A string token spans both quotes; use lz_token_string_value to get
 * its decoded contents.
 */
typedef struct LzToken {
    LzTokenType type;
    uint32_t offset;
    uint32_t length;
    union {
        int64_t integer;
        double number;
        const char *error;
    } value;
} LzToken;

typedef struct LzLexer {
    const LzSource *source;
    size_t position;
    unsigned nesting;
    bool after_newline;
} LzLexer;

/* The source must outlive the lexer and be shorter than LZ_NO_OFFSET bytes. */
void lz_lexer_init(LzLexer *lexer, const LzSource *source);

/*
 * Returns the next token and never fails: malformed input becomes an ERROR
 * token and scanning resumes after it, so one pass reports every error. After
 * the last token it returns EOF forever.
 *
 * Newlines end statements, so they are tokens, but runs of blank or comment-only
 * lines produce one NEWLINE, none is produced at the start of the file, and a
 * final one is added before EOF if the last line has none. Line breaks inside
 * ( ) and [ ] are ignored so calls and lists can span lines.
 */
LzToken lz_lexer_next(LzLexer *lexer);

const char *lz_token_type_name(LzTokenType type);

/*
 * Appends the decoded contents of a STRING token to out, which the caller
 * frees with lz_buffer_free. Use out->length; out->data stays NULL while the
 * contents are empty. Returns false on allocation failure.
 */
bool lz_token_string_value(const LzSource *source, const LzToken *token, LzBuffer *out);

#endif
