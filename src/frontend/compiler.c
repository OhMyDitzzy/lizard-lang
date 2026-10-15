#include "lizard/compiler.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lizard/lexer.h"
#include "lizard/native.h"

#define LZ_MAX_SYMBOLS 512
#define LZ_MAX_LOCALS 256
#define LZ_MAX_NESTING 128
#define LZ_MAX_ARGUMENTS 255

typedef enum SymbolKind {
    SYMBOL_LOCAL,    /* stack slot, declared inside a block */
    SYMBOL_GLOBAL,   /* top-level let, or a const whose value is only known at run time */
    SYMBOL_CONSTANT  /* const folded at compile time: no storage, uses emit the value */
} SymbolKind;

typedef struct Symbol {
    LzString *name;
    int depth;
    SymbolKind kind;
    bool is_const;
    uint16_t slot;
    LzValue value;
} Symbol;

typedef struct Parser {
    LzVM *vm;
    const LzSource *source;
    LzChunk *chunk;
    LzLexer lexer;
    LzToken current;
    LzToken previous;
    LzDiagnostics *diagnostics;
    bool panic;
    LzStatus fatal;
    Symbol symbols[LZ_MAX_SYMBOLS];
    size_t symbol_count;
    size_t local_count;
    int scope_depth;
    int nesting;
} Parser;

typedef enum Precedence {
    PREC_NONE,
    PREC_OR,
    PREC_AND,
    PREC_EQUALITY,
    PREC_COMPARISON,
    PREC_TERM,
    PREC_FACTOR,
    PREC_UNARY,
    PREC_CALL
} Precedence;

typedef void (*ParseFn)(Parser *parser);

typedef struct ParseRule {
    ParseFn prefix;
    ParseFn infix;
    Precedence precedence;
} ParseRule;

static void expression(Parser *parser);
static void declaration(Parser *parser);
static void parse_precedence(Parser *parser, Precedence precedence);

/* Diagnostics */

static void record(Parser *parser, uint32_t offset, uint32_t length, const char *message) {
    LzDiagnostics *diagnostics = parser->diagnostics;
    if (diagnostics->count >= LZ_MAX_DIAGNOSTICS) {
        diagnostics->dropped++;
        return;
    }
    LzDiagnostic *item = &diagnostics->items[diagnostics->count++];
    item->offset = offset;
    item->length = length;
    (void)snprintf(item->message, sizeof item->message, "%s", message);
}

static void error_at(Parser *parser, const LzToken *token, const char *message) {
    if (parser->panic) {
        return;
    }
    parser->panic = true;
    record(parser, token->offset, token->length, message);
}

static void error_at_current(Parser *parser, const char *message) {
    error_at(parser, &parser->current, message);
}

static void error_at_previous(Parser *parser, const char *message) {
    error_at(parser, &parser->previous, message);
}

static void token_text(const Parser *parser, const LzToken *token, char *out, size_t size) {
    size_t length = token->length < size - 1 ? token->length : size - 1;
    memcpy(out, parser->source->text + token->offset, length);
    out[length] = '\0';
}

static void error_with_name(Parser *parser, const LzToken *token, const char *before,
                            const char *after) {
    char name[48];
    char message[128];
    token_text(parser, token, name, sizeof name);
    (void)snprintf(message, sizeof message, "%s%s%s", before, name, after);
    error_at(parser, token, message);
}

static void fail(Parser *parser, LzStatus status) {
    if (parser->fatal == LZ_OK) {
        parser->fatal = status;
    }
}

/* Token handling */

static void advance(Parser *parser) {
    parser->previous = parser->current;
    for (;;) {
        parser->current = lz_lexer_next(&parser->lexer);
        if (parser->current.type != LZ_TOKEN_ERROR) {
            break;
        }
        record(parser, parser->current.offset, parser->current.length,
               parser->current.value.error);
        parser->panic = true;
    }
}

static bool check(const Parser *parser, LzTokenType type) {
    return parser->current.type == type;
}

static bool match(Parser *parser, LzTokenType type) {
    if (!check(parser, type)) {
        return false;
    }
    advance(parser);
    return true;
}

static void consume(Parser *parser, LzTokenType type, const char *message) {
    if (check(parser, type)) {
        advance(parser);
    } else {
        error_at_current(parser, message);
    }
}

static void skip_newlines(Parser *parser) {
    while (check(parser, LZ_TOKEN_NEWLINE)) {
        advance(parser);
    }
}

/* Emitting */

static void emit_byte_at(Parser *parser, uint8_t byte, uint32_t offset) {
    if (!lz_chunk_write(parser->chunk, byte, offset)) {
        fail(parser, LZ_ERR_MEMORY);
    }
}

static void emit_op_at(Parser *parser, LzOpCode op, uint32_t offset) {
    emit_byte_at(parser, (uint8_t)op, offset);
}

static void emit_op(Parser *parser, LzOpCode op) {
    emit_op_at(parser, op, parser->previous.offset);
}

static void emit_u16_at(Parser *parser, uint16_t operand, uint32_t offset) {
    emit_byte_at(parser, (uint8_t)(operand >> 8), offset);
    emit_byte_at(parser, (uint8_t)(operand & 0xFF), offset);
}

static uint16_t make_constant(Parser *parser, LzValue value) {
    if (parser->chunk->constant_count >= LZ_CONSTANTS_MAX) {
        error_at_previous(parser, "too many constants in one chunk");
        return 0;
    }
    uint16_t index = 0;
    if (!lz_chunk_add_constant(parser->chunk, value, &index)) {
        fail(parser, LZ_ERR_MEMORY);
    }
    return index;
}

static void emit_constant(Parser *parser, LzValue value) {
    uint32_t offset = parser->previous.offset;
    uint16_t index = make_constant(parser, value);
    emit_op_at(parser, LZ_OP_CONSTANT, offset);
    emit_u16_at(parser, index, offset);
}

static LzString *intern_token(Parser *parser, const LzToken *token) {
    LzString *name =
        lz_vm_intern(parser->vm, parser->source->text + token->offset, token->length);
    if (name == NULL) {
        fail(parser, LZ_ERR_MEMORY);
    }
    return name;
}

static void emit_named(Parser *parser, LzOpCode op, LzString *name, uint32_t offset) {
    uint16_t index = make_constant(parser, lz_value_object(&name->base));
    emit_op_at(parser, op, offset);
    emit_u16_at(parser, index, offset);
}

static size_t emit_jump(Parser *parser, LzOpCode op, uint32_t offset) {
    emit_op_at(parser, op, offset);
    emit_u16_at(parser, 0xFFFF, offset);
    return parser->chunk->count - 2;
}

static void patch_jump(Parser *parser, size_t operand_index) {
    size_t distance = parser->chunk->count - (operand_index + 2);
    if (distance > UINT16_MAX) {
        error_at_previous(parser, "jump too large");
        return;
    }
    parser->chunk->code[operand_index] = (uint8_t)(distance >> 8);
    parser->chunk->code[operand_index + 1] = (uint8_t)(distance & 0xFF);
}

static void emit_loop(Parser *parser, size_t loop_start, uint32_t offset) {
    emit_op_at(parser, LZ_OP_LOOP, offset);
    size_t distance = parser->chunk->count + 2 - loop_start;
    if (distance > UINT16_MAX) {
        error_at_previous(parser, "loop body too large");
        distance = 0;
    }
    emit_u16_at(parser, (uint16_t)distance, offset);
}

/* Symbols */

static Symbol *find_symbol(Parser *parser, const LzString *name) {
    for (size_t i = parser->symbol_count; i > 0; i--) {
        if (parser->symbols[i - 1].name == name) {
            return &parser->symbols[i - 1];
        }
    }
    return NULL;
}

static Symbol *add_symbol(Parser *parser, const LzToken *token, LzString *name, SymbolKind kind,
                          bool is_const, LzValue value) {
    for (size_t i = parser->symbol_count; i > 0; i--) {
        const Symbol *existing = &parser->symbols[i - 1];
        if (existing->depth < parser->scope_depth) {
            break;
        }
        if (existing->name == name) {
            error_with_name(parser, token, "variable '", "' is already declared in this scope");
            return NULL;
        }
    }
    if (parser->symbol_count >= LZ_MAX_SYMBOLS) {
        error_at(parser, token, "too many variables");
        return NULL;
    }
    if (kind == SYMBOL_LOCAL && parser->local_count >= LZ_MAX_LOCALS) {
        error_at(parser, token, "too many local variables");
        return NULL;
    }

    Symbol *symbol = &parser->symbols[parser->symbol_count++];
    symbol->name = name;
    symbol->depth = parser->scope_depth;
    symbol->kind = kind;
    symbol->is_const = is_const;
    symbol->slot = kind == SYMBOL_LOCAL ? (uint16_t)parser->local_count++ : 0;
    symbol->value = value;
    return symbol;
}

static void begin_scope(Parser *parser) {
    parser->scope_depth++;
}

static void end_scope(Parser *parser) {
    uint32_t offset = parser->previous.offset;
    while (parser->symbol_count > 0 &&
           parser->symbols[parser->symbol_count - 1].depth == parser->scope_depth) {
        Symbol *symbol = &parser->symbols[--parser->symbol_count];
        if (symbol->kind == SYMBOL_LOCAL) {
            emit_op_at(parser, LZ_OP_POP, offset);
            parser->local_count--;
        } else if (symbol->kind == SYMBOL_CONSTANT) {
            lz_value_release(&parser->vm->heap, &symbol->value);
        }
    }
    parser->scope_depth--;
}

/* Expressions */

static void number(Parser *parser) {
    if (parser->previous.type == LZ_TOKEN_INTEGER) {
        emit_constant(parser, lz_value_int(parser->previous.value.integer));
    } else {
        emit_constant(parser, lz_value_float(parser->previous.value.number));
    }
}

static void string(Parser *parser) {
    LzBuffer decoded = {0};
    if (!lz_token_string_value(parser->source, &parser->previous, &decoded)) {
        lz_buffer_free(&decoded);
        fail(parser, LZ_ERR_MEMORY);
        return;
    }

    LzString *text = lz_string_new(&parser->vm->heap, decoded.data, decoded.length);
    lz_buffer_free(&decoded);
    if (text == NULL) {
        fail(parser, lz_vm_out_of_memory(parser->vm));
        return;
    }
    emit_constant(parser, lz_value_object(&text->base));
    lz_object_release(&parser->vm->heap, &text->base);
}

static void literal(Parser *parser) {
    switch (parser->previous.type) {
    case LZ_TOKEN_TRUE:
        emit_op(parser, LZ_OP_TRUE);
        break;
    case LZ_TOKEN_FALSE:
        emit_op(parser, LZ_OP_FALSE);
        break;
    default:
        emit_op(parser, LZ_OP_NIL);
        break;
    }
}

static void grouping(Parser *parser) {
    expression(parser);
    consume(parser, LZ_TOKEN_RIGHT_PAREN, "expected ')' after expression");
}

static void variable(Parser *parser) {
    LzToken token = parser->previous;
    LzString *name = intern_token(parser, &token);
    if (name == NULL) {
        return;
    }

    Symbol *symbol = find_symbol(parser, name);
    if (symbol != NULL) {
        switch (symbol->kind) {
        case SYMBOL_LOCAL:
            emit_op_at(parser, LZ_OP_GET_LOCAL, token.offset);
            emit_u16_at(parser, symbol->slot, token.offset);
            break;
        case SYMBOL_GLOBAL:
            emit_named(parser, LZ_OP_GET_GLOBAL, name, token.offset);
            break;
        case SYMBOL_CONSTANT:
            emit_constant(parser, symbol->value);
            break;
        }
    } else if (lz_vm_lookup(parser->vm, name) != NULL) {
        emit_named(parser, LZ_OP_GET_GLOBAL, name, token.offset);
    } else {
        error_with_name(parser, &token, "undefined variable '", "'");
    }
}

static void unary(Parser *parser) {
    LzToken op = parser->previous;
    parse_precedence(parser, PREC_UNARY);
    emit_op_at(parser, op.type == LZ_TOKEN_BANG ? LZ_OP_NOT : LZ_OP_NEGATE, op.offset);
}

static void binary(Parser *parser);
static void call(Parser *parser);
static void and_(Parser *parser);
static void or_(Parser *parser);

static ParseRule rule_for(LzTokenType type) {
    switch (type) {
    case LZ_TOKEN_INTEGER:
    case LZ_TOKEN_FLOAT:
        return (ParseRule){number, NULL, PREC_NONE};
    case LZ_TOKEN_STRING:
        return (ParseRule){string, NULL, PREC_NONE};
    case LZ_TOKEN_TRUE:
    case LZ_TOKEN_FALSE:
    case LZ_TOKEN_NIL:
        return (ParseRule){literal, NULL, PREC_NONE};
    case LZ_TOKEN_IDENTIFIER:
        return (ParseRule){variable, NULL, PREC_NONE};
    case LZ_TOKEN_LEFT_PAREN:
        return (ParseRule){grouping, call, PREC_CALL};
    case LZ_TOKEN_BANG:
        return (ParseRule){unary, NULL, PREC_NONE};
    case LZ_TOKEN_MINUS:
        return (ParseRule){unary, binary, PREC_TERM};
    case LZ_TOKEN_PLUS:
        return (ParseRule){NULL, binary, PREC_TERM};
    case LZ_TOKEN_STAR:
    case LZ_TOKEN_SLASH:
    case LZ_TOKEN_SLASH_SLASH:
    case LZ_TOKEN_PERCENT:
        return (ParseRule){NULL, binary, PREC_FACTOR};
    case LZ_TOKEN_EQUAL_EQUAL:
    case LZ_TOKEN_BANG_EQUAL:
        return (ParseRule){NULL, binary, PREC_EQUALITY};
    case LZ_TOKEN_LESS:
    case LZ_TOKEN_LESS_EQUAL:
    case LZ_TOKEN_GREATER:
    case LZ_TOKEN_GREATER_EQUAL:
        return (ParseRule){NULL, binary, PREC_COMPARISON};
    case LZ_TOKEN_AND:
        return (ParseRule){NULL, and_, PREC_AND};
    case LZ_TOKEN_OR:
        return (ParseRule){NULL, or_, PREC_OR};
    default:
        return (ParseRule){NULL, NULL, PREC_NONE};
    }
}

static void parse_precedence(Parser *parser, Precedence precedence) {
    if (parser->fatal != LZ_OK) {
        return;
    }
    if (parser->nesting >= LZ_MAX_NESTING) {
        error_at_current(parser, "expression nested too deeply");
        return;
    }

    ParseFn prefix = rule_for(parser->current.type).prefix;
    if (prefix == NULL) {
        error_at_current(parser, "expected expression");
        return;
    }

    parser->nesting++;
    advance(parser);
    prefix(parser);
    while (parser->fatal == LZ_OK && precedence <= rule_for(parser->current.type).precedence) {
        ParseFn infix = rule_for(parser->current.type).infix;
        advance(parser);
        infix(parser);
    }
    parser->nesting--;
}

static void expression(Parser *parser) {
    parse_precedence(parser, PREC_OR);
}

static void binary(Parser *parser) {
    LzToken op = parser->previous;
    ParseRule rule = rule_for(op.type);
    skip_newlines(parser);
    parse_precedence(parser, (Precedence)(rule.precedence + 1));

    switch (op.type) {
    case LZ_TOKEN_PLUS:
        emit_op_at(parser, LZ_OP_ADD, op.offset);
        break;
    case LZ_TOKEN_MINUS:
        emit_op_at(parser, LZ_OP_SUBTRACT, op.offset);
        break;
    case LZ_TOKEN_STAR:
        emit_op_at(parser, LZ_OP_MULTIPLY, op.offset);
        break;
    case LZ_TOKEN_SLASH:
        emit_op_at(parser, LZ_OP_DIVIDE, op.offset);
        break;
    case LZ_TOKEN_SLASH_SLASH:
        emit_op_at(parser, LZ_OP_FLOOR_DIVIDE, op.offset);
        break;
    case LZ_TOKEN_PERCENT:
        emit_op_at(parser, LZ_OP_MODULO, op.offset);
        break;
    case LZ_TOKEN_EQUAL_EQUAL:
        emit_op_at(parser, LZ_OP_EQUAL, op.offset);
        break;
    case LZ_TOKEN_BANG_EQUAL:
        emit_op_at(parser, LZ_OP_EQUAL, op.offset);
        emit_op_at(parser, LZ_OP_NOT, op.offset);
        break;
    case LZ_TOKEN_LESS:
        emit_op_at(parser, LZ_OP_LESS, op.offset);
        break;
    case LZ_TOKEN_LESS_EQUAL:
        emit_op_at(parser, LZ_OP_LESS_EQUAL, op.offset);
        break;
    case LZ_TOKEN_GREATER:
        emit_op_at(parser, LZ_OP_GREATER, op.offset);
        break;
    default:
        emit_op_at(parser, LZ_OP_GREATER_EQUAL, op.offset);
        break;
    }
}

static void call(Parser *parser) {
    uint32_t offset = parser->previous.offset;
    unsigned count = 0;

    if (!check(parser, LZ_TOKEN_RIGHT_PAREN)) {
        do {
            if (check(parser, LZ_TOKEN_RIGHT_PAREN)) {
                break;
            }
            expression(parser);
            if (++count > LZ_MAX_ARGUMENTS) {
                error_at_previous(parser, "too many arguments");
                count = LZ_MAX_ARGUMENTS;
            }
        } while (match(parser, LZ_TOKEN_COMMA));
    }
    consume(parser, LZ_TOKEN_RIGHT_PAREN, "expected ')' after arguments");

    emit_op_at(parser, LZ_OP_CALL, offset);
    emit_byte_at(parser, (uint8_t)count, offset);
}

static void and_(Parser *parser) {
    uint32_t offset = parser->previous.offset;
    size_t end_jump = emit_jump(parser, LZ_OP_JUMP_IF_FALSE, offset);
    emit_op_at(parser, LZ_OP_POP, offset);
    skip_newlines(parser);
    parse_precedence(parser, PREC_AND);
    patch_jump(parser, end_jump);
}

static void or_(Parser *parser) {
    uint32_t offset = parser->previous.offset;
    size_t end_jump = emit_jump(parser, LZ_OP_JUMP_IF_TRUE, offset);
    emit_op_at(parser, LZ_OP_POP, offset);
    skip_newlines(parser);
    parse_precedence(parser, PREC_OR);
    patch_jump(parser, end_jump);
}

/* Compile-time constants */

static bool fragment_is_pure(const LzChunk *chunk, size_t start) {
    size_t i = start;
    while (i < chunk->count) {
        uint8_t op = chunk->code[i];
        switch (op) {
        case LZ_OP_CONSTANT:
        case LZ_OP_NIL:
        case LZ_OP_TRUE:
        case LZ_OP_FALSE:
        case LZ_OP_ADD:
        case LZ_OP_SUBTRACT:
        case LZ_OP_MULTIPLY:
        case LZ_OP_DIVIDE:
        case LZ_OP_FLOOR_DIVIDE:
        case LZ_OP_MODULO:
        case LZ_OP_NEGATE:
        case LZ_OP_EQUAL:
        case LZ_OP_LESS:
        case LZ_OP_LESS_EQUAL:
        case LZ_OP_GREATER:
        case LZ_OP_GREATER_EQUAL:
        case LZ_OP_NOT:
            break;
        default:
            return false;
        }
        i += lz_opcode_length(op);
    }
    return true;
}

/* Runs the code emitted since start on the real VM and returns its result. */
static LzStatus evaluate_fragment(Parser *parser, size_t start, LzValue *value) {
    const LzChunk *chunk = parser->chunk;
    size_t size = chunk->count - start;

    uint8_t *code = malloc(size + 1);
    uint32_t *offsets = malloc((size + 1) * sizeof *offsets);
    if (code == NULL || offsets == NULL) {
        free(code);
        free(offsets);
        return LZ_ERR_MEMORY;
    }
    memcpy(code, chunk->code + start, size);
    memcpy(offsets, chunk->offsets + start, size * sizeof *offsets);
    code[size] = (uint8_t)LZ_OP_RETURN;
    offsets[size] = offsets[size - 1];

    /* The constant table is shared, not copied, so temp must never be freed as a chunk. */
    LzChunk temp = {code,
                    offsets,
                    size + 1,
                    size + 1,
                    chunk->constants,
                    chunk->constant_count,
                    chunk->constant_count};

    LzVM *vm = parser->vm;
    uint64_t fuel = vm->fuel;
    uint64_t instructions = vm->instructions;
    vm->fuel = LZ_FUEL_UNLIMITED;
    LzStatus status = lz_vm_eval(vm, &temp, value);
    vm->fuel = fuel;
    vm->instructions = instructions;

    free(code);
    free(offsets);
    return status;
}

/* Statements */

static void end_statement(Parser *parser) {
    if (match(parser, LZ_TOKEN_NEWLINE) || match(parser, LZ_TOKEN_SEMICOLON)) {
        return;
    }
    if (check(parser, LZ_TOKEN_EOF) || check(parser, LZ_TOKEN_RIGHT_BRACE)) {
        return;
    }
    if (check(parser, LZ_TOKEN_EQUAL)) {
        error_at_current(parser, "invalid assignment target");
    } else {
        error_at_current(parser, "expected end of statement");
    }
}

static void declare_variable(Parser *parser, const LzToken *name_token, LzString *name,
                             bool is_const) {
    if (parser->scope_depth == 0) {
        Symbol *symbol = add_symbol(parser, name_token, name, SYMBOL_GLOBAL, is_const,
                                    lz_value_nil());
        if (symbol != NULL) {
            emit_named(parser, LZ_OP_DEFINE_GLOBAL, name, name_token->offset);
        }
    } else {
        add_symbol(parser, name_token, name, SYMBOL_LOCAL, is_const, lz_value_nil());
    }
}

static void variable_declaration(Parser *parser, bool is_const) {
    if (!check(parser, LZ_TOKEN_IDENTIFIER)) {
        error_at_current(parser, is_const ? "expected constant name after 'const'"
                                          : "expected variable name after 'let'");
        return;
    }
    advance(parser);
    LzToken name_token = parser->previous;
    LzString *name = intern_token(parser, &name_token);
    if (name == NULL) {
        return;
    }

    size_t code_mark = parser->chunk->count;
    size_t constant_mark = parser->chunk->constant_count;
    LzToken initializer_start = parser->current;
    bool has_initializer = false;

    if (match(parser, LZ_TOKEN_EQUAL)) {
        skip_newlines(parser);
        initializer_start = parser->current;
        expression(parser);
        has_initializer = true;
    } else if (is_const) {
        error_at_current(parser, "expected '=' after constant name");
        return;
    } else {
        emit_op_at(parser, LZ_OP_NIL, name_token.offset);
    }
    end_statement(parser);

    if (is_const && has_initializer && !parser->panic && parser->fatal == LZ_OK &&
        parser->chunk->count > code_mark && fragment_is_pure(parser->chunk, code_mark)) {
        LzValue value = lz_value_nil();
        LzStatus status = evaluate_fragment(parser, code_mark, &value);
        if (status == LZ_OK) {
            lz_chunk_truncate(&parser->vm->heap, parser->chunk, code_mark, constant_mark);
            if (add_symbol(parser, &name_token, name, SYMBOL_CONSTANT, true, value) == NULL) {
                lz_value_release(&parser->vm->heap, &value);
            }
            return;
        }
        if (status == LZ_ERR_MEMORY || status == LZ_ERR_HEAP_LIMIT) {
            fail(parser, status);
            return;
        }

        LzToken span = initializer_start;
        span.length = parser->previous.offset + parser->previous.length - span.offset;
        error_at(parser, &span, parser->vm->error);
    }

    declare_variable(parser, &name_token, name, is_const);
}

static void assignment_statement(Parser *parser) {
    advance(parser);
    LzToken name_token = parser->previous;
    LzString *name = intern_token(parser, &name_token);
    advance(parser);
    skip_newlines(parser);
    expression(parser);
    if (name == NULL) {
        return;
    }

    Symbol *symbol = find_symbol(parser, name);
    if (symbol == NULL) {
        if (lz_vm_lookup(parser->vm, name) != NULL) {
            error_with_name(parser, &name_token, "cannot assign to builtin '", "'");
        } else {
            error_with_name(parser, &name_token, "undefined variable '", "'");
        }
    } else if (symbol->is_const || symbol->kind == SYMBOL_CONSTANT) {
        error_with_name(parser, &name_token, "cannot assign to constant '", "'");
    } else if (symbol->kind == SYMBOL_LOCAL) {
        emit_op_at(parser, LZ_OP_SET_LOCAL, name_token.offset);
        emit_u16_at(parser, symbol->slot, name_token.offset);
    } else {
        emit_named(parser, LZ_OP_SET_GLOBAL, name, name_token.offset);
    }
    end_statement(parser);
}

static void block(Parser *parser) {
    if (parser->nesting >= LZ_MAX_NESTING) {
        error_at_current(parser, "blocks nested too deeply");
        return;
    }
    parser->nesting++;
    consume(parser, LZ_TOKEN_LEFT_BRACE, "expected '{'");
    begin_scope(parser);
    while (!check(parser, LZ_TOKEN_RIGHT_BRACE) && !check(parser, LZ_TOKEN_EOF) &&
           parser->fatal == LZ_OK) {
        declaration(parser);
    }
    consume(parser, LZ_TOKEN_RIGHT_BRACE, "expected '}' after block");
    end_scope(parser);
    parser->nesting--;
}

static void if_statement(Parser *parser) {
    uint32_t offset = parser->previous.offset;
    expression(parser);
    if (!check(parser, LZ_TOKEN_LEFT_BRACE)) {
        error_at_current(parser, "expected '{' after condition");
        return;
    }

    size_t then_jump = emit_jump(parser, LZ_OP_JUMP_IF_FALSE, offset);
    emit_op_at(parser, LZ_OP_POP, offset);
    block(parser);
    size_t else_jump = emit_jump(parser, LZ_OP_JUMP, offset);
    patch_jump(parser, then_jump);
    emit_op_at(parser, LZ_OP_POP, offset);

    skip_newlines(parser);
    if (match(parser, LZ_TOKEN_ELSE)) {
        if (match(parser, LZ_TOKEN_IF)) {
            if_statement(parser);
        } else if (check(parser, LZ_TOKEN_LEFT_BRACE)) {
            block(parser);
        } else {
            error_at_current(parser, "expected '{' or 'if' after 'else'");
        }
    }
    patch_jump(parser, else_jump);
}

static void while_statement(Parser *parser) {
    uint32_t offset = parser->previous.offset;
    size_t loop_start = parser->chunk->count;
    expression(parser);
    if (!check(parser, LZ_TOKEN_LEFT_BRACE)) {
        error_at_current(parser, "expected '{' after condition");
        return;
    }

    size_t exit_jump = emit_jump(parser, LZ_OP_JUMP_IF_FALSE, offset);
    emit_op_at(parser, LZ_OP_POP, offset);
    block(parser);
    emit_loop(parser, loop_start, offset);
    patch_jump(parser, exit_jump);
    emit_op_at(parser, LZ_OP_POP, offset);
}

static bool next_token_is_equal(const Parser *parser) {
    LzLexer ahead = parser->lexer;
    LzToken token = lz_lexer_next(&ahead);
    return token.type == LZ_TOKEN_EQUAL;
}

static void expression_statement(Parser *parser) {
    uint32_t offset = parser->current.offset;
    expression(parser);
    emit_op_at(parser, LZ_OP_POP, offset);
    end_statement(parser);
}

static void synchronize(Parser *parser) {
    parser->panic = false;
    while (!check(parser, LZ_TOKEN_EOF)) {
        if (parser->previous.type == LZ_TOKEN_NEWLINE ||
            parser->previous.type == LZ_TOKEN_SEMICOLON) {
            return;
        }
        switch (parser->current.type) {
        case LZ_TOKEN_LET:
        case LZ_TOKEN_CONST:
        case LZ_TOKEN_IF:
        case LZ_TOKEN_WHILE:
        case LZ_TOKEN_RIGHT_BRACE:
            return;
        default:
            break;
        }
        advance(parser);
    }
}

static void declaration(Parser *parser) {
    if (parser->fatal != LZ_OK) {
        return;
    }
    if (match(parser, LZ_TOKEN_NEWLINE) || match(parser, LZ_TOKEN_SEMICOLON)) {
        return;
    }

    uint32_t start_offset = parser->current.offset;
    LzTokenType start_type = parser->current.type;

    switch (parser->current.type) {
    case LZ_TOKEN_LET:
        advance(parser);
        variable_declaration(parser, false);
        break;
    case LZ_TOKEN_CONST:
        advance(parser);
        variable_declaration(parser, true);
        break;
    case LZ_TOKEN_IF:
        advance(parser);
        if_statement(parser);
        break;
    case LZ_TOKEN_WHILE:
        advance(parser);
        while_statement(parser);
        break;
    case LZ_TOKEN_LEFT_BRACE:
        block(parser);
        break;
    case LZ_TOKEN_RIGHT_BRACE:
        error_at_current(parser, "unexpected '}'");
        break;
    case LZ_TOKEN_IDENTIFIER:
        if (next_token_is_equal(parser)) {
            assignment_statement(parser);
        } else {
            expression_statement(parser);
        }
        break;
    default:
        expression_statement(parser);
        break;
    }

    if (parser->panic) {
        synchronize(parser);
        /* A stray closing brace is consumed here; inside a block it ends the block instead. */
        if (parser->current.offset == start_offset && parser->current.type == start_type &&
            !check(parser, LZ_TOKEN_EOF)) {
            advance(parser);
        }
    }
}

/* Entry points */

static LzStatus compile_into(Parser *parser) {
    advance(parser);
    while (!check(parser, LZ_TOKEN_EOF) && parser->fatal == LZ_OK) {
        declaration(parser);
    }
    emit_op_at(parser, LZ_OP_RETURN, parser->current.offset);

    for (size_t i = 0; i < parser->symbol_count; i++) {
        if (parser->symbols[i].kind == SYMBOL_CONSTANT) {
            lz_value_release(&parser->vm->heap, &parser->symbols[i].value);
        }
    }

    if (parser->fatal != LZ_OK) {
        return parser->fatal;
    }
    return parser->diagnostics->count > 0 ? LZ_ERR_COMPILE : LZ_OK;
}

LzStatus lz_compile(LzVM *vm, const LzSource *source, LzChunk *chunk, LzDiagnostics *diagnostics) {
    diagnostics->count = 0;
    diagnostics->dropped = 0;

    Parser *parser = calloc(1, sizeof *parser);
    if (parser == NULL) {
        return lz_vm_set_error(vm, LZ_ERR_MEMORY, "out of memory");
    }
    parser->vm = vm;
    parser->source = source;
    parser->chunk = chunk;
    parser->diagnostics = diagnostics;
    lz_lexer_init(&parser->lexer, source);

    LzStatus status = compile_into(parser);
    if (status == LZ_ERR_MEMORY || status == LZ_ERR_HEAP_LIMIT) {
        if (vm->error[0] == '\0') {
            (void)lz_vm_out_of_memory(vm);
        }
    }
    free(parser);
    return status;
}

void lz_diagnostics_write(const LzDiagnostics *diagnostics, const LzSource *source,
                          const LzWriter *out) {
    for (size_t i = 0; i < diagnostics->count; i++) {
        const LzDiagnostic *item = &diagnostics->items[i];
        lz_source_write_diagnostic(source, item->offset, item->length, "error", item->message,
                                   out);
    }
    if (diagnostics->dropped > 0) {
        char line[64];
        (void)snprintf(line, sizeof line, "... and %zu more error(s)\n", diagnostics->dropped);
        (void)lz_writer_write_cstr(out, line);
    }
}
