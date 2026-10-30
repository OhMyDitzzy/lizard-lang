#include "lizard/compiler.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lizard/array.h"
#include "lizard/function.h"
#include "lizard/lexer.h"
#include "lizard/native.h"
#include "lizard/report.h"
#include "lizard/suggest.h"
#include "lizard/type.h"

#define LZ_MAX_SYMBOLS 512
#define LZ_MAX_LOCALS 256
#define LZ_MAX_NESTING 128
#define LZ_MAX_ARGUMENTS 255

typedef enum SymbolKind {
    SYMBOL_LOCAL,    /* stack slot, declared inside a block */
    SYMBOL_GLOBAL,   /* top-level let, or a const whose value is only known at run time */
    SYMBOL_CONSTANT  /* const folded at compile time: no storage, uses emit the value */
} SymbolKind;

struct FuncState;

/*
 * What the compiler knows about a declared function: enough to check a call of it
 * at compile time (argument count, names, types) and to type its result.
 */
typedef struct FuncSig {
    struct FuncSig *next; /* every signature is freed together when compilation ends */
    LzString *fname;      /* NULL for an anonymous function */
    uint16_t param_count;
    uint16_t required;
    bool has_rest;
    LzType result; /* the declared result type, or any */
    bool result_declared;
    LzType rest_item; /* type of each surplus argument */
    LzString *names[LZ_MAX_PARAMS + 1];
    LzType types[LZ_MAX_PARAMS];
} FuncSig;

typedef struct Symbol {
    LzString *name;
    int depth;
    SymbolKind kind;
    bool is_const;
    uint16_t slot;
    LzValue value;
    LzType type; /* declared type, or the exact type of a folded const */
    struct FuncState *owner; /* the function whose frame holds a local */
    bool captured;           /* a closure captured this local: closing its scope must close upvalues */
    bool pending;            /* a top-level `fn` seen ahead of its declaration */
    const FuncSig *sig;      /* set for a declared function: calls of it are checked */
} Symbol;

#define LZ_MAX_ALIASES 64

typedef struct Alias {
    LzString *name;
    int depth;
    LzType type;
} Alias;

/*
 * Identical constants of a chunk are stored once, so a program that mentions one
 * variable or literal many times does not run out of the 65536 slots of a chunk.
 * The index is an open-addressing table of constant indexes plus one, kept at
 * most half full. It is exact: truncating the chunk after a folded const removes
 * the entries of the dropped constants (stale entries for a value that keeps
 * coming back would pile up into one long probe chain).
 */
typedef struct ConstantIndex {
    uint32_t *slots;
    size_t capacity; /* a power of two, or 0 until the first constant */
    size_t used;     /* occupied slots */
} ConstantIndex;

/*
 * The function being compiled. The parser's chunk, constant index and local count
 * always belong to the innermost one; the enclosing function's are parked here.
 */
typedef struct FuncState {
    struct FuncState *enclosing;
    LzFunction *function; /* NULL for the top-level script */
    size_t first_symbol;  /* symbols from here on belong to this function */
    size_t self_symbol;   /* the symbol of a `fn name` declared in the enclosing scope, or SIZE_MAX */
    const FuncSig *sig;
    LzUpvalueDesc upvalues[LZ_MAX_UPVALUES];
    size_t upvalue_count;
    LzChunk *saved_chunk;
    ConstantIndex saved_constants;
    size_t saved_local_count;
    bool saved_terminated;
} FuncState;

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
    ConstantIndex constants;
    Alias aliases[LZ_MAX_ALIASES];
    size_t alias_count;
    FuncState *fs;      /* innermost function being compiled */
    FuncSig *sigs;      /* every signature made so far */
    bool terminated;    /* the statement just compiled never falls through (return, ...) */
    bool method_name;   /* the identifier being compiled follows a '.' (receiver.name(...)) */
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

/*
 * What the compiler knows about the value an expression leaves on the stack:
 * its static type, and the builtin when the expression is a direct reference to
 * one, so a call can use that builtin's result type.
 */
typedef struct Expr {
    LzType type;
    const LzNative *native;
    const FuncSig *sig; /* the expression is a direct reference to a declared function */
    bool is_index;  /* the expression is `a[i]` and its last instruction is OP_INDEX... */
    size_t op_pos;  /* ...at this code offset, so a following '=' can turn it into OP_SET_INDEX */
} Expr;

typedef Expr (*PrefixFn)(Parser *parser);
typedef Expr (*InfixFn)(Parser *parser, Expr left);

typedef struct ParseRule {
    PrefixFn prefix;
    InfixFn infix;
    Precedence precedence;
} ParseRule;

static Expr expression(Parser *parser);
static void declaration(Parser *parser);
static LzTokenType next_token_type(const Parser *parser);
static void end_statement(Parser *parser);
static Expr function_expression(Parser *parser);
static Expr parse_precedence(Parser *parser, Precedence precedence);

/* Diagnostics */

static void record_full(Parser *parser, uint32_t offset, uint32_t length, const char *message,
                        const char *label, const LzSuggestions *suggestions) {
    LzDiagnostics *diagnostics = parser->diagnostics;
    if (diagnostics->count >= LZ_MAX_DIAGNOSTICS) {
        diagnostics->dropped++;
        return;
    }
    LzDiagnostic *item = &diagnostics->items[diagnostics->count++];
    item->offset = offset;
    item->length = length;
    (void)snprintf(item->message, sizeof item->message, "%s", message);
    item->label = label;
    if (suggestions != NULL) {
        item->suggestions = *suggestions;
    } else {
        item->suggestions.count = 0;
    }
}

static void record(Parser *parser, uint32_t offset, uint32_t length, const char *message) {
    record_full(parser, offset, length, message, NULL, NULL);
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

/* Like error_with_name, with a label under the name and "did you mean" candidates. */
static void error_with_name_suggest(Parser *parser, const LzToken *token, const char *before,
                                    const char *after, const char *label,
                                    const LzSuggestions *suggestions) {
    if (parser->panic) {
        return;
    }
    char name[48];
    char message[128];
    token_text(parser, token, name, sizeof name);
    (void)snprintf(message, sizeof message, "%s%s%s", before, name, after);
    parser->panic = true;
    record_full(parser, token->offset, token->length, message, label, suggestions);
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

static uint32_t hash_constant(const LzValue *value) {
    uint64_t bits = 0;
    switch (value->type) {
    case LZ_VAL_NIL:
        break;
    case LZ_VAL_BOOL:
        bits = value->as.boolean ? 1u : 0u;
        break;
    case LZ_VAL_INT:
        bits = (uint64_t)value->as.integer;
        break;
    case LZ_VAL_FLOAT:
        memcpy(&bits, &value->as.number, sizeof bits);
        break;
    case LZ_VAL_OBJECT:
        if (value->as.object->type == LZ_OBJ_STRING) {
            return ((const LzString *)value->as.object)->hash;
        }
        bits = (uint64_t)(uintptr_t)value->as.object;
        break;
    }
    bits += (uint64_t)value->type;
    bits ^= bits >> 33;
    bits *= 0xFF51AFD7ED558CCDull;
    bits ^= bits >> 33;
    return (uint32_t)bits;
}

/*
 * Names must match by identity: GET_GLOBAL needs the interned string itself, and
 * a string literal with the same text is a different object. Everything else
 * matches by value (floats by their bits, so 0.0 and -0.0 stay apart).
 */
static bool constants_match(const LzValue *a, const LzValue *b, bool by_identity) {
    if (a->type != b->type) {
        return false;
    }
    switch (a->type) {
    case LZ_VAL_NIL:
        return true;
    case LZ_VAL_BOOL:
        return a->as.boolean == b->as.boolean;
    case LZ_VAL_INT:
        return a->as.integer == b->as.integer;
    case LZ_VAL_FLOAT:
        return memcmp(&a->as.number, &b->as.number, sizeof a->as.number) == 0;
    case LZ_VAL_OBJECT:
        if (a->as.object == b->as.object) {
            return true;
        }
        if (by_identity || a->as.object->type != LZ_OBJ_STRING ||
            b->as.object->type != LZ_OBJ_STRING) {
            return false;
        }
        const LzString *x = (const LzString *)a->as.object;
        const LzString *y = (const LzString *)b->as.object;
        return x->length == y->length && memcmp(x->data, y->data, x->length) == 0;
    }
    return false;
}

static bool find_constant(const Parser *parser, const LzValue *value, bool by_identity,
                          uint16_t *index) {
    const ConstantIndex *table = &parser->constants;
    if (table->capacity == 0) {
        return false;
    }
    const LzChunk *chunk = parser->chunk;
    size_t mask = table->capacity - 1;
    for (size_t slot = hash_constant(value) & mask; table->slots[slot] != 0;
         slot = (slot + 1) & mask) {
        size_t candidate = table->slots[slot] - 1u;
        if (candidate < chunk->constant_count &&
            constants_match(&chunk->constants[candidate], value, by_identity)) {
            *index = (uint16_t)candidate;
            return true;
        }
    }
    return false;
}

static bool rebuild_constant_index(Parser *parser, size_t capacity) {
    uint32_t *slots = calloc(capacity, sizeof *slots);
    if (slots == NULL) {
        return false;
    }
    const LzChunk *chunk = parser->chunk;
    size_t mask = capacity - 1;
    for (size_t i = 0; i < chunk->constant_count; i++) {
        size_t slot = hash_constant(&chunk->constants[i]) & mask;
        while (slots[slot] != 0) {
            slot = (slot + 1) & mask;
        }
        slots[slot] = (uint32_t)(i + 1);
    }
    free(parser->constants.slots);
    parser->constants.slots = slots;
    parser->constants.capacity = capacity;
    parser->constants.used = chunk->constant_count;
    return true;
}

/* The constant at index was just added: record it so later uses share it. */
static void remember_constant(Parser *parser, uint16_t index) {
    ConstantIndex *table = &parser->constants;
    if ((table->used + 1) * 2 > table->capacity) {
        /* Grow, or build the index again after a failed allocation. It covers every live constant. */
        size_t capacity = table->capacity != 0 ? table->capacity : 64;
        while ((parser->chunk->constant_count + 1) * 2 > capacity) {
            capacity *= 2;
        }
        if (!rebuild_constant_index(parser, capacity)) {
            /* Sharing is only an optimization, so carry on without an index. */
            free(table->slots);
            table->slots = NULL;
            table->capacity = 0;
            table->used = 0;
        }
        return;
    }
    size_t mask = table->capacity - 1;
    size_t slot = hash_constant(&parser->chunk->constants[index]) & mask;
    while (table->slots[slot] != 0) {
        slot = (slot + 1) & mask;
    }
    table->slots[slot] = (uint32_t)index + 1u;
    table->used++;
}

/* Drops the entry of the constant at index (backward-shift deletion: no tombstones). */
static void forget_constant(Parser *parser, size_t index) {
    ConstantIndex *table = &parser->constants;
    if (table->capacity == 0) {
        return;
    }
    const LzChunk *chunk = parser->chunk;
    size_t mask = table->capacity - 1;
    size_t hole = hash_constant(&chunk->constants[index]) & mask;
    while (table->slots[hole] != index + 1) {
        if (table->slots[hole] == 0) {
            return; /* never indexed */
        }
        hole = (hole + 1) & mask;
    }
    for (size_t next = (hole + 1) & mask; table->slots[next] != 0; next = (next + 1) & mask) {
        size_t home = hash_constant(&chunk->constants[table->slots[next] - 1]) & mask;
        /* The entry may move back into the hole unless its home lies in (hole, next]. */
        bool stays = hole <= next ? (hole < home && home <= next) : (hole < home || home <= next);
        if (!stays) {
            table->slots[hole] = table->slots[next];
            hole = next;
        }
    }
    table->slots[hole] = 0;
    table->used--;
}

/* Call before the chunk drops the constants from index `from` on. */
static void forget_constants_from(Parser *parser, size_t from) {
    for (size_t i = parser->chunk->constant_count; i > from; i--) {
        forget_constant(parser, i - 1);
    }
}

static uint16_t add_constant(Parser *parser, LzValue value, bool by_identity) {
    uint16_t index = 0;
    if (find_constant(parser, &value, by_identity, &index)) {
        return index;
    }
    if (parser->chunk->constant_count >= LZ_CONSTANTS_MAX) {
        error_at_previous(parser, "too many constants in one chunk");
        return 0;
    }
    if (!lz_chunk_add_constant(parser->chunk, value, &index)) {
        fail(parser, LZ_ERR_MEMORY);
        return index;
    }
    remember_constant(parser, index);
    return index;
}

static uint16_t make_constant(Parser *parser, LzValue value) {
    return add_constant(parser, value, false);
}

static uint16_t emit_constant(Parser *parser, LzValue value) {
    uint32_t offset = parser->previous.offset;
    uint16_t index = make_constant(parser, value);
    emit_op_at(parser, LZ_OP_CONSTANT, offset);
    emit_u16_at(parser, index, offset);
    return index;
}

static LzString *intern_token(Parser *parser, const LzToken *token) {
    LzString *name =
        lz_vm_intern(parser->vm, parser->source->text + token->offset, token->length);
    if (name == NULL) {
        fail(parser, lz_vm_out_of_memory(parser->vm));
    }
    return name;
}

static void emit_named(Parser *parser, LzOpCode op, LzString *name, uint32_t offset) {
    uint16_t index = add_constant(parser, lz_value_object(&name->base), true);
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
                          bool is_const, LzValue value, const LzType *type) {
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
    symbol->type = *type;
    symbol->owner = parser->fs;
    symbol->captured = false;
    symbol->pending = false;
    symbol->sig = NULL;
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
            emit_op_at(parser, symbol->captured ? LZ_OP_CLOSE_UPVALUE : LZ_OP_POP, offset);
            parser->local_count--;
        } else if (symbol->kind == SYMBOL_CONSTANT) {
            lz_value_release(&parser->vm->heap, &symbol->value);
        }
    }
    while (parser->alias_count > 0 &&
           parser->aliases[parser->alias_count - 1].depth == parser->scope_depth) {
        parser->alias_count--;
    }
    parser->scope_depth--;
}

/* Static types */

static Expr expr_of(LzType type) {
    Expr expr;
    expr.type = type;
    expr.native = NULL;
    expr.sig = NULL;
    expr.is_index = false;
    expr.op_pos = 0;
    return expr;
}

static Expr any_expr(void) {
    return expr_of(lz_type_of_kinds(LZ_KINDS_ANY));
}

static void describe_type(const LzType *type, char *out, size_t size) {
    LzBuffer buffer = {0};
    LzWriter writer = lz_writer_from_buffer(&buffer);
    lz_type_describe(type, &writer);
    (void)snprintf(out, size, "%s", buffer.data != NULL ? buffer.data : "?");
    lz_buffer_free(&buffer);
}

static LzToken span_from(const Parser *parser, const LzToken *start) {
    LzToken span = *start;
    uint32_t end = parser->previous.offset + parser->previous.length;
    span.length = end > span.offset ? end - span.offset : 0;
    return span;
}

/* "<prefix><a><middle><b>", with b and its middle part left out when b is NULL. */
static void error_types(Parser *parser, const LzToken *at, const char *prefix, const LzType *a,
                        const char *middle, const LzType *b) {
    char first[96];
    char second[96];
    char message[256];
    describe_type(a, first, sizeof first);
    second[0] = '\0';
    if (b != NULL) {
        describe_type(b, second, sizeof second);
    }
    (void)snprintf(message, sizeof message, "%s%s%s%s", prefix, first, middle, second);
    error_at(parser, at, message);
}

static void error_operator(Parser *parser, const LzToken *op, const LzType *left,
                           const LzType *right) {
    char symbol[16];
    char first[96];
    char second[96];
    char message[256];
    token_text(parser, op, symbol, sizeof symbol);
    describe_type(left, first, sizeof first);
    if (right == NULL) {
        (void)snprintf(message, sizeof message, "operator '%s' cannot be applied to %s", symbol,
                       first);
    } else {
        describe_type(right, second, sizeof second);
        (void)snprintf(message, sizeof message, "operator '%s' cannot be applied to %s and %s",
                       symbol, first, second);
    }
    error_at(parser, op, message);
}

/*
 * Checks a value of type from against the type it is stored under. A value that
 * always fits needs nothing, one that never fits is a compile error, and one
 * that might fit (any, a union, a wider type) gets a run-time check.
 */
static void check_flow(Parser *parser, const LzType *from, const LzType *to, const LzToken *start,
                       uint32_t offset, LzOpCode check_op, const char *mismatch) {
    switch (lz_type_assignable(from, to)) {
    case LZ_ASSIGN_ALWAYS:
        /*
         * An array stored under `T[]` is checked and restricted to T even when it
         * fits statically, so later pushes and stores into it are checked too.
         */
        if ((from->kinds & LZ_KIND_ARRAY) == 0 || (to->kinds & LZ_KIND_ARRAY) == 0 ||
            to->element == NULL) {
            return;
        }
        break;
    case LZ_ASSIGN_NEVER: {
        LzToken span = span_from(parser, start);
        error_types(parser, &span, mismatch, to, ", got ", from);
        return;
    }
    case LZ_ASSIGN_MAYBE:
        break;
    }

    uint16_t index = 0;
    if (!lz_chunk_add_type(parser->chunk, to, &index)) {
        if (parser->chunk->type_count >= LZ_TYPES_MAX) {
            error_at_previous(parser, "too many types in one chunk");
        } else {
            fail(parser, LZ_ERR_MEMORY);
        }
        return;
    }
    emit_op_at(parser, check_op, offset);
    emit_u16_at(parser, index, offset);
}

static void check_assignment(Parser *parser, const LzType *from, const LzType *to,
                             const LzToken *start, uint32_t offset) {
    check_flow(parser, from, to, start, offset, LZ_OP_CHECK_TYPE, "type mismatch: expected ");
}

static Expr union_expr(Expr left, Expr right) {
    LzType merged;
    if (!lz_type_union(&merged, &left.type, &right.type)) {
        LzType a = lz_type_widen(&left.type);
        LzType b = lz_type_widen(&right.type);
        (void)lz_type_union(&merged, &a, &b);
    }
    return expr_of(merged);
}

/* The type `item[]`; a limit or memory error is reported once and gives `any[]`. */
static LzType array_type(Parser *parser, const LzType *item) {
    LzType out = lz_type_of_kinds(LZ_KIND_ARRAY);
    LzStatus status = lz_vm_array_of(parser->vm, item, &out);
    if (status == LZ_ERR_MEMORY || status == LZ_ERR_HEAP_LIMIT) {
        fail(parser, status);
    } else if (status != LZ_OK) {
        error_at_previous(parser, parser->vm->error);
    }
    if (status != LZ_OK) {
        out = lz_type_of_kinds(LZ_KIND_ARRAY);
    }
    return out;
}

/* Type syntax */

static bool builtin_type(const char *name, LzType *out) {
    if (strcmp(name, "int") == 0) {
        *out = lz_type_of_kinds(LZ_KIND_INT);
    } else if (strcmp(name, "float") == 0) {
        *out = lz_type_of_kinds(LZ_KIND_FLOAT);
    } else if (strcmp(name, "bool") == 0) {
        *out = lz_type_of_kinds(LZ_KIND_BOOL);
    } else if (strcmp(name, "string") == 0) {
        *out = lz_type_of_kinds(LZ_KIND_STRING);
    } else if (strcmp(name, "number") == 0) {
        *out = lz_type_of_kinds(LZ_KINDS_NUMBER);
    } else if (strcmp(name, "any") == 0) {
        *out = lz_type_of_kinds(LZ_KINDS_ANY);
    } else if (strcmp(name, "never") == 0) {
        *out = lz_type_of_kinds(0);
    } else if (strcmp(name, "function") == 0) {
        *out = lz_type_of_kinds(LZ_KIND_FUNCTION);
    } else {
        return false;
    }
    return true;
}

static const Alias *find_alias(const Parser *parser, const LzString *name) {
    for (size_t i = parser->alias_count; i > 0; i--) {
        if (parser->aliases[i - 1].name == name) {
            return &parser->aliases[i - 1];
        }
    }
    return NULL;
}

/* Suggestions: names close to one the program used and that does not exist. */

static void offer_table(LzSuggester *suggester, const LzTable *table, bool functions_only) {
    for (size_t i = 0; i < table->capacity; i++) {
        const LzTableEntry *entry = &table->entries[i];
        if (entry->key == NULL) {
            continue;
        }
        if (functions_only && (lz_kind_of(&entry->value) & LZ_KIND_FUNCTION) == 0) {
            continue;
        }
        lz_suggester_offer(suggester, entry->key->data);
    }
}

/*
 * Everything a name could have meant here: the locals and functions in scope, then
 * the globals and builtins of the VM. When the name was used as a call, only
 * things that can hold a function are offered. Names an `import` will bring in
 * become symbols or globals too, so they join this list without any change here.
 */
static LzSuggestions suggest_names(Parser *parser, const LzToken *token, bool functions_only) {
    LzSuggester suggester;
    lz_suggester_init(&suggester, parser->source->text + token->offset, token->length);
    for (size_t i = parser->symbol_count; i > 0; i--) { /* innermost scope first */
        const Symbol *symbol = &parser->symbols[i - 1];
        if (functions_only && (lz_type_possible_kinds(&symbol->type) & LZ_KIND_FUNCTION) == 0) {
            continue;
        }
        lz_suggester_offer(&suggester, symbol->name->data);
    }
    offer_table(&suggester, &parser->vm->globals, functions_only);
    offer_table(&suggester, &parser->vm->builtins, functions_only);
    if (parser->method_name) {
        offer_table(&suggester, &parser->vm->methods, functions_only);
    }
    return suggester.found;
}

static const char *const BUILTIN_TYPE_NAMES[] = {"int",    "float", "bool", "string",
                                                 "number", "any",   "never", "function"};

static LzSuggestions suggest_types(Parser *parser, const LzToken *token) {
    LzSuggester suggester;
    lz_suggester_init(&suggester, parser->source->text + token->offset, token->length);
    for (size_t i = 0; i < sizeof BUILTIN_TYPE_NAMES / sizeof BUILTIN_TYPE_NAMES[0]; i++) {
        lz_suggester_offer(&suggester, BUILTIN_TYPE_NAMES[i]);
    }
    for (size_t i = parser->alias_count; i > 0; i--) {
        lz_suggester_offer(&suggester, parser->aliases[i - 1].name->data);
    }
    return suggester.found;
}

static LzSuggestions suggest_parameters(const Parser *parser, const LzToken *token,
                                        const FuncSig *sig) {
    LzSuggester suggester;
    lz_suggester_init(&suggester, parser->source->text + token->offset, token->length);
    for (size_t i = 0; i < sig->param_count; i++) {
        if (sig->names[i] != NULL) {
            lz_suggester_offer(&suggester, sig->names[i]->data);
        }
    }
    return suggester.found;
}

/* Remembers the candidates of an undefined name for the run-time error that may follow. */
static void record_name_hint(Parser *parser, const LzToken *token, bool is_call,
                             bool is_assignment, bool is_method) {
    LzDiagnostics *diagnostics = parser->diagnostics;
    if (diagnostics->hint_count >= LZ_MAX_NAME_HINTS) {
        return;
    }
    LzNameHint *hint = &diagnostics->hints[diagnostics->hint_count++];
    hint->offset = token->offset;
    hint->length = token->length;
    hint->is_call = is_call;
    hint->is_assignment = is_assignment;
    hint->is_method = is_method;
    hint->suggestions = suggest_names(parser, token, is_call);
}

static LzType literal_type(const LzLiteral *literal) {
    LzType type = lz_type_of_kinds(0);
    (void)lz_type_add_literal(&type, literal);
    return type;
}

static bool parse_type_union(Parser *parser, LzType *out);

static bool parse_type_primary(Parser *parser, LzType *out) {
    *out = lz_type_of_kinds(LZ_KINDS_ANY);
    if (parser->nesting >= LZ_MAX_NESTING) {
        error_at_current(parser, "type nested too deeply");
        return false;
    }

    switch (parser->current.type) {
    case LZ_TOKEN_IDENTIFIER: {
        advance(parser);
        LzToken token = parser->previous;
        char name[48];
        token_text(parser, &token, name, sizeof name);
        if (builtin_type(name, out)) {
            return true;
        }
        LzString *id = intern_token(parser, &token);
        if (id == NULL) {
            return false;
        }
        const Alias *alias = find_alias(parser, id);
        if (alias == NULL) {
            LzSuggestions close = suggest_types(parser, &token);
            error_with_name_suggest(parser, &token, "unknown type '", "'", "not a known type",
                                    &close);
            return false;
        }
        *out = alias->type;
        return true;
    }
    case LZ_TOKEN_NIL:
        advance(parser);
        *out = lz_type_of_kinds(LZ_KIND_NIL);
        return true;
    case LZ_TOKEN_FN:
        advance(parser);
        if (check(parser, LZ_TOKEN_LEFT_PAREN)) {
            error_at_current(parser, "function types with a signature are not supported yet; write 'fn'");
            return false;
        }
        *out = lz_type_of_kinds(LZ_KIND_FUNCTION);
        return true;
    case LZ_TOKEN_TRUE:
    case LZ_TOKEN_FALSE: {
        advance(parser);
        LzLiteral literal = {.kind = LZ_VAL_BOOL,
                             .as.boolean = parser->previous.type == LZ_TOKEN_TRUE};
        *out = literal_type(&literal);
        return true;
    }
    case LZ_TOKEN_STRING: {
        advance(parser);
        LzBuffer decoded = {0};
        if (!lz_token_string_value(parser->source, &parser->previous, &decoded)) {
            lz_buffer_free(&decoded);
            fail(parser, LZ_ERR_MEMORY);
            return false;
        }
        LzString *text = lz_vm_intern(parser->vm, decoded.data != NULL ? decoded.data : "",
                                      decoded.length);
        lz_buffer_free(&decoded);
        if (text == NULL) {
            fail(parser, lz_vm_out_of_memory(parser->vm));
            return false;
        }
        LzLiteral literal = {.kind = LZ_VAL_OBJECT, .as.string = text};
        *out = literal_type(&literal);
        return true;
    }
    case LZ_TOKEN_INTEGER: {
        advance(parser);
        if (parser->previous.value.integer == INT64_MIN) {
            error_at_previous(parser, "integer literal out of range");
            return false;
        }
        LzLiteral literal = {.kind = LZ_VAL_INT, .as.integer = parser->previous.value.integer};
        *out = literal_type(&literal);
        return true;
    }
    case LZ_TOKEN_MINUS: {
        advance(parser);
        if (!check(parser, LZ_TOKEN_INTEGER)) {
            error_at_current(parser, "expected a number after '-' in a type");
            return false;
        }
        advance(parser);
        int64_t magnitude = parser->previous.value.integer;
        LzLiteral literal = {.kind = LZ_VAL_INT,
                             .as.integer = magnitude == INT64_MIN ? INT64_MIN : -magnitude};
        *out = literal_type(&literal);
        return true;
    }
    case LZ_TOKEN_FLOAT:
        advance(parser);
        error_at_previous(parser, "float literal types are not supported");
        return false;
    case LZ_TOKEN_LEFT_PAREN: {
        advance(parser);
        parser->nesting++;
        bool ok = parse_type_union(parser, out);
        parser->nesting--;
        consume(parser, LZ_TOKEN_RIGHT_PAREN, "expected ')' after type");
        return ok;
    }
    default:
        error_at_current(parser, "expected a type");
        return false;
    }
}

static bool parse_type_postfix(Parser *parser, LzType *out) {
    bool ok = parse_type_primary(parser, out);
    for (;;) {
        if (match(parser, LZ_TOKEN_QUESTION)) {
            LzType nil = lz_type_of_kinds(LZ_KIND_NIL);
            LzType merged;
            if (lz_type_union(&merged, out, &nil)) {
                *out = merged;
            }
        } else if (check(parser, LZ_TOKEN_LEFT_BRACKET)) {
            advance(parser);
            consume(parser, LZ_TOKEN_RIGHT_BRACKET, "expected ']' in an array type");
            LzType item = *out;
            *out = array_type(parser, &item);
        } else {
            break;
        }
    }
    return ok;
}

/* type = postfix ( "|" postfix )*   where postfix = primary "?"* */
static bool parse_type_union(Parser *parser, LzType *out) {
    bool ok = parse_type_postfix(parser, out);
    while (match(parser, LZ_TOKEN_PIPE)) {
        skip_newlines(parser);
        LzType next;
        ok = parse_type_postfix(parser, &next) && ok;
        LzType merged;
        if ((out->kinds & next.kinds & LZ_KIND_ARRAY) != 0 && out->element != next.element) {
            error_at_previous(parser, "a union cannot hold two different array types; "
                                      "write (A | B)[] instead");
            ok = false;
        }
        if (!lz_type_union(&merged, out, &next)) {
            error_at_previous(parser, "too many literal types in one union (limit 8)");
            LzType a = lz_type_widen(out);
            LzType b = lz_type_widen(&next);
            (void)lz_type_union(&merged, &a, &b);
            ok = false;
        }
        *out = merged;
    }
    return ok;
}

/* Expressions */

static Expr number(Parser *parser) {
    if (parser->previous.type == LZ_TOKEN_INTEGER) {
        /* The lexer returns the magnitude 2^63 as INT64_MIN; only unary() accepts it. */
        if (parser->previous.value.integer == INT64_MIN) {
            error_at_previous(parser, "integer literal out of range");
            return any_expr();
        }
        LzValue value = lz_value_int(parser->previous.value.integer);
        emit_constant(parser, value);
        return expr_of(lz_type_of_value(&value));
    }
    emit_constant(parser, lz_value_float(parser->previous.value.number));
    return expr_of(lz_type_of_kinds(LZ_KIND_FLOAT));
}

static Expr string(Parser *parser) {
    LzBuffer decoded = {0};
    if (!lz_token_string_value(parser->source, &parser->previous, &decoded)) {
        lz_buffer_free(&decoded);
        fail(parser, LZ_ERR_MEMORY);
        return any_expr();
    }

    LzString *text = lz_string_new(&parser->vm->heap, decoded.data, decoded.length);
    lz_buffer_free(&decoded);
    if (text == NULL) {
        fail(parser, lz_vm_out_of_memory(parser->vm));
        return any_expr();
    }
    uint16_t index = emit_constant(parser, lz_value_object(&text->base));
    lz_object_release(&parser->vm->heap, &text->base);

    if (index < parser->chunk->constant_count) {
        return expr_of(lz_type_of_value(&parser->chunk->constants[index]));
    }
    return expr_of(lz_type_of_kinds(LZ_KIND_STRING));
}

static Expr literal(Parser *parser) {
    switch (parser->previous.type) {
    case LZ_TOKEN_TRUE:
        emit_op(parser, LZ_OP_TRUE);
        return expr_of(lz_type_of_value(&(LzValue){.type = LZ_VAL_BOOL, .as.boolean = true}));
    case LZ_TOKEN_FALSE:
        emit_op(parser, LZ_OP_FALSE);
        return expr_of(lz_type_of_value(&(LzValue){.type = LZ_VAL_BOOL, .as.boolean = false}));
    default:
        emit_op(parser, LZ_OP_NIL);
        return expr_of(lz_type_of_kinds(LZ_KIND_NIL));
    }
}

static Expr grouping(Parser *parser) {
    Expr inner = expression(parser);
    consume(parser, LZ_TOKEN_RIGHT_PAREN, "expected ')' after expression");
    return inner;
}

static int add_upvalue(Parser *parser, FuncState *fs, uint16_t index, bool is_local) {
    for (size_t i = 0; i < fs->upvalue_count; i++) {
        if (fs->upvalues[i].index == index && fs->upvalues[i].is_local == is_local) {
            return (int)i;
        }
    }
    if (fs->upvalue_count >= LZ_MAX_UPVALUES) {
        error_at_previous(parser, "too many variables captured by one function");
        return -1;
    }
    fs->upvalues[fs->upvalue_count] = (LzUpvalueDesc){index, is_local};
    return (int)fs->upvalue_count++;
}

/*
 * The index of the upvalue through which fs reaches a local of an enclosing
 * function, creating it (and one in every function in between). -1 after an error.
 */
static int resolve_upvalue(Parser *parser, FuncState *fs, Symbol *symbol) {
    if (symbol->owner == fs->enclosing) {
        symbol->captured = true;
        return add_upvalue(parser, fs, symbol->slot, true);
    }
    if (fs->enclosing == NULL) {
        return -1;
    }
    int outer = resolve_upvalue(parser, fs->enclosing, symbol);
    return outer < 0 ? -1 : add_upvalue(parser, fs, (uint16_t)outer, false);
}

static Expr variable(Parser *parser) {
    LzToken token = parser->previous;
    LzString *name = intern_token(parser, &token);
    if (name == NULL) {
        return any_expr();
    }

    Symbol *symbol = find_symbol(parser, name);
    if (symbol != NULL) {
        switch (symbol->kind) {
        case SYMBOL_LOCAL:
            if (symbol->owner == parser->fs) {
                emit_op_at(parser, LZ_OP_GET_LOCAL, token.offset);
                emit_u16_at(parser, symbol->slot, token.offset);
            } else if (parser->fs->self_symbol != SIZE_MAX &&
                       symbol == &parser->symbols[parser->fs->self_symbol]) {
                /* A local function calling itself: no capture, so no reference cycle. */
                emit_op_at(parser, LZ_OP_GET_SELF, token.offset);
            } else {
                int index = resolve_upvalue(parser, parser->fs, symbol);
                emit_op_at(parser, LZ_OP_GET_UPVALUE, token.offset);
                emit_u16_at(parser, (uint16_t)(index < 0 ? 0 : index), token.offset);
            }
            break;
        case SYMBOL_GLOBAL:
            emit_named(parser, LZ_OP_GET_GLOBAL, name, token.offset);
            break;
        case SYMBOL_CONSTANT:
            emit_constant(parser, symbol->value);
            break;
        }
        Expr found = expr_of(symbol->type);
        found.sig = symbol->sig;
        return found;
    }

    const LzValue *defined = lz_vm_lookup(parser->vm, name);
    if (defined == NULL) {
        /*
         * Sequential execution: a name that is not defined yet is not a compile error. The
         * program runs statement by statement and fails, with the source position, at the
         * statement that really uses a name that does not exist when it runs.
         */
        record_name_hint(parser, &token, check(parser, LZ_TOKEN_LEFT_PAREN), false,
                        parser->method_name);
        emit_named(parser, LZ_OP_GET_GLOBAL, name, token.offset);
        return any_expr();
    }

    emit_named(parser, LZ_OP_GET_GLOBAL, name, token.offset);
    Expr expr = any_expr();
    if (defined->type == LZ_VAL_OBJECT && defined->as.object->type == LZ_OBJ_NATIVE) {
        expr.type = lz_type_of_kinds(LZ_KIND_FUNCTION);
        expr.native = (const LzNative *)defined->as.object;
    }
    return expr;
}

static Expr unary(Parser *parser) {
    LzToken op = parser->previous;
    if (op.type == LZ_TOKEN_MINUS && check(parser, LZ_TOKEN_INTEGER) &&
        parser->current.value.integer == INT64_MIN) {
        /* -9223372036854775808: the one literal that only exists after a minus sign. */
        advance(parser);
        emit_constant(parser, lz_value_int(INT64_MIN));
        return expr_of(lz_type_of_kinds(LZ_KIND_INT));
    }

    Expr operand = parse_precedence(parser, PREC_UNARY);
    emit_op_at(parser, op.type == LZ_TOKEN_BANG ? LZ_OP_NOT : LZ_OP_NEGATE, op.offset);
    if (op.type == LZ_TOKEN_BANG) {
        return expr_of(lz_type_of_kinds(LZ_KIND_BOOL));
    }

    uint16_t numbers = (uint16_t)(lz_type_possible_kinds(&operand.type) & LZ_KINDS_NUMBER);
    if (numbers == 0) {
        error_operator(parser, &op, &operand.type, NULL);
        return any_expr();
    }
    return expr_of(lz_type_of_kinds(numbers));
}

/* [a, b, c]: every literal builds a fresh array, so it is never folded into a constant. */
static Expr array_literal(Parser *parser) {
    uint32_t offset = parser->previous.offset;
    emit_op_at(parser, LZ_OP_ARRAY_NEW, offset);

    LzType item = lz_type_of_kinds(0);
    if (!check(parser, LZ_TOKEN_RIGHT_BRACKET)) {
        do {
            if (check(parser, LZ_TOKEN_RIGHT_BRACKET)) {
                break; /* trailing comma */
            }
            Expr value = expression(parser);
            emit_op_at(parser, LZ_OP_ARRAY_PUSH, offset);
            LzType widened = lz_type_widen(&value.type);
            LzType merged;
            if (lz_type_union(&merged, &item, &widened)) {
                item = merged;
            }
        } while (match(parser, LZ_TOKEN_COMMA));
    }
    consume(parser, LZ_TOKEN_RIGHT_BRACKET, "expected ']' after array items");
    return expr_of(array_type(parser, &item));
}

/* The static type of one item of an array expression. */
static LzType item_type_of(const LzType *container) {
    if ((container->kinds & LZ_KIND_ARRAY) != 0 && container->element != NULL) {
        return *container->element;
    }
    return lz_type_of_kinds(LZ_KINDS_ANY);
}

static void check_bound(Parser *parser, const LzToken *at, const Expr *bound, bool slice) {
    uint16_t allowed = slice ? (uint16_t)(LZ_KIND_INT | LZ_KIND_NIL) : (uint16_t)LZ_KIND_INT;
    if ((lz_type_possible_kinds(&bound->type) & allowed) == 0) {
        error_types(parser, at,
                    slice ? "a slice bound must be an int, got " : "an array index must be an int, got ",
                    &bound->type, "", NULL);
    }
}

/* a[i] and a[i:j]; either bound of a slice may be left out. */
static Expr subscript(Parser *parser, Expr left) {
    LzToken bracket = parser->previous;
    uint32_t offset = bracket.offset;

    if ((lz_type_possible_kinds(&left.type) & LZ_KIND_ARRAY) == 0) {
        error_types(parser, &bracket, "cannot index a value of type ", &left.type, "", NULL);
    }

    bool has_first = !check(parser, LZ_TOKEN_COLON);
    Expr first = expr_of(lz_type_of_kinds(LZ_KIND_NIL));
    if (has_first) {
        first = expression(parser);
    } else {
        emit_op_at(parser, LZ_OP_NIL, offset);
    }

    if (match(parser, LZ_TOKEN_COLON)) {
        if (has_first) {
            check_bound(parser, &bracket, &first, true);
        }
        if (check(parser, LZ_TOKEN_RIGHT_BRACKET)) {
            emit_op_at(parser, LZ_OP_NIL, offset);
        } else {
            Expr last = expression(parser);
            check_bound(parser, &bracket, &last, true);
        }
        consume(parser, LZ_TOKEN_RIGHT_BRACKET, "expected ']' after slice");
        emit_op_at(parser, LZ_OP_SLICE, offset);
        if ((left.type.kinds & LZ_KIND_ARRAY) == 0) {
            return any_expr();
        }
        LzType slice = lz_type_of_kinds(LZ_KIND_ARRAY);
        slice.element = left.type.element;
        return expr_of(slice);
    }

    check_bound(parser, &bracket, &first, false);
    consume(parser, LZ_TOKEN_RIGHT_BRACKET, "expected ']' after index");
    Expr result = expr_of(item_type_of(&left.type));
    result.is_index = true;
    result.op_pos = parser->chunk->count;
    emit_op_at(parser, LZ_OP_INDEX, offset);
    return result;
}

static Expr binary(Parser *parser, Expr left);
static Expr call(Parser *parser, Expr callee);
static Expr dot_call(Parser *parser, Expr receiver);
static Expr and_(Parser *parser, Expr left);
static Expr or_(Parser *parser, Expr left);

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
    case LZ_TOKEN_LEFT_BRACKET:
        return (ParseRule){array_literal, subscript, PREC_CALL};
    case LZ_TOKEN_DOT:
        return (ParseRule){NULL, dot_call, PREC_CALL};
    case LZ_TOKEN_FN:
        return (ParseRule){function_expression, NULL, PREC_NONE};
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

static Expr parse_precedence(Parser *parser, Precedence precedence) {
    if (parser->fatal != LZ_OK) {
        return any_expr();
    }
    if (parser->nesting >= LZ_MAX_NESTING) {
        error_at_current(parser, "expression nested too deeply");
        return any_expr();
    }

    PrefixFn prefix = rule_for(parser->current.type).prefix;
    if (prefix == NULL) {
        error_at_current(parser, "expected expression");
        return any_expr();
    }

    parser->nesting++;
    advance(parser);
    Expr left = prefix(parser);
    for (;;) {
        /* A chain may continue on the next line:  xs\n  .filter(...)\n  .map(...) */
        if (check(parser, LZ_TOKEN_NEWLINE) && next_token_type(parser) == LZ_TOKEN_DOT) {
            advance(parser);
        }
        if (parser->fatal != LZ_OK || precedence > rule_for(parser->current.type).precedence) {
            break;
        }
        InfixFn infix = rule_for(parser->current.type).infix;
        advance(parser);
        left = infix(parser, left);
    }
    parser->nesting--;
    return left;
}

static Expr expression(Parser *parser) {
    return parse_precedence(parser, PREC_OR);
}

/* The type of an arithmetic result, or a compile error when no operand pairing can work. */
static Expr arithmetic_result(Parser *parser, const LzToken *op, const Expr *left,
                              const Expr *right) {
    uint16_t a = lz_type_possible_kinds(&left->type);
    uint16_t b = lz_type_possible_kinds(&right->type);
    bool numbers = (a & LZ_KINDS_NUMBER) != 0 && (b & LZ_KINDS_NUMBER) != 0;
    bool strings = op->type == LZ_TOKEN_PLUS && (a & LZ_KIND_STRING) != 0 &&
                   (b & LZ_KIND_STRING) != 0;
    bool arrays = op->type == LZ_TOKEN_PLUS && (a & LZ_KIND_ARRAY) != 0 &&
                  (b & LZ_KIND_ARRAY) != 0;
    if (!numbers && !strings && !arrays) {
        error_operator(parser, op, &left->type, &right->type);
        return any_expr();
    }

    uint16_t kinds = 0;
    if (numbers) {
        uint16_t an = (uint16_t)(a & LZ_KINDS_NUMBER);
        uint16_t bn = (uint16_t)(b & LZ_KINDS_NUMBER);
        if (op->type == LZ_TOKEN_SLASH) {
            kinds = LZ_KIND_FLOAT;
        } else if (an == LZ_KIND_INT && bn == LZ_KIND_INT) {
            kinds = LZ_KIND_INT;
        } else if (an == LZ_KIND_FLOAT || bn == LZ_KIND_FLOAT) {
            kinds = LZ_KIND_FLOAT;
        } else {
            kinds = LZ_KINDS_NUMBER;
        }
    }
    if (strings) {
        kinds = (uint16_t)(kinds | LZ_KIND_STRING);
    }
    LzType result = lz_type_of_kinds(kinds);
    if (arrays) {
        result.kinds = (uint16_t)(result.kinds | LZ_KIND_ARRAY);
        const LzType *x = left->type.element;
        const LzType *y = right->type.element;
        if (x == y) {
            result.element = x;
        } else if (x != NULL && y != NULL) {
            /* int[] + string[] holds both: (int | string)[] */
            LzType merged;
            if (lz_type_union(&merged, x, y)) {
                result.element = array_type(parser, &merged).element;
            }
        }
    }
    return expr_of(result);
}

static Expr binary(Parser *parser, Expr left) {
    LzToken op = parser->previous;
    ParseRule rule = rule_for(op.type);
    skip_newlines(parser);
    Expr right = parse_precedence(parser, (Precedence)(rule.precedence + 1));

    switch (op.type) {
    case LZ_TOKEN_PLUS:
        emit_op_at(parser, LZ_OP_ADD, op.offset);
        return arithmetic_result(parser, &op, &left, &right);
    case LZ_TOKEN_MINUS:
        emit_op_at(parser, LZ_OP_SUBTRACT, op.offset);
        return arithmetic_result(parser, &op, &left, &right);
    case LZ_TOKEN_STAR:
        emit_op_at(parser, LZ_OP_MULTIPLY, op.offset);
        return arithmetic_result(parser, &op, &left, &right);
    case LZ_TOKEN_SLASH:
        emit_op_at(parser, LZ_OP_DIVIDE, op.offset);
        return arithmetic_result(parser, &op, &left, &right);
    case LZ_TOKEN_SLASH_SLASH:
        emit_op_at(parser, LZ_OP_FLOOR_DIVIDE, op.offset);
        return arithmetic_result(parser, &op, &left, &right);
    case LZ_TOKEN_PERCENT:
        emit_op_at(parser, LZ_OP_MODULO, op.offset);
        return arithmetic_result(parser, &op, &left, &right);
    case LZ_TOKEN_EQUAL_EQUAL:
        emit_op_at(parser, LZ_OP_EQUAL, op.offset);
        return expr_of(lz_type_of_kinds(LZ_KIND_BOOL));
    case LZ_TOKEN_BANG_EQUAL:
        emit_op_at(parser, LZ_OP_EQUAL, op.offset);
        emit_op_at(parser, LZ_OP_NOT, op.offset);
        return expr_of(lz_type_of_kinds(LZ_KIND_BOOL));
    default:
        break;
    }

    switch (op.type) {
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

    uint16_t a = lz_type_possible_kinds(&left.type);
    uint16_t b = lz_type_possible_kinds(&right.type);
    if ((a & LZ_KINDS_NUMBER) == 0 || (b & LZ_KINDS_NUMBER) == 0) {
        error_operator(parser, &op, &left.type, &right.type);
    }
    return expr_of(lz_type_of_kinds(LZ_KIND_BOOL));
}

static bool is_any_type(const LzType *type) {
    return type->kinds == LZ_KINDS_ANY && type->literal_count == 0 && type->element == NULL;
}

/* Is the argument list that starts at the current token one with `*spread` or `name = value`? */
static bool call_has_special_args(const Parser *parser) {
    LzLexer ahead = parser->lexer;
    LzToken token = parser->current;
    int depth = 0;
    bool at_start = true;
    for (;;) {
        if (token.type == LZ_TOKEN_EOF) {
            return false;
        }
        if (depth == 0) {
            if (token.type == LZ_TOKEN_RIGHT_PAREN) {
                return false;
            }
            if (at_start) {
                if (token.type == LZ_TOKEN_STAR) {
                    return true;
                }
                if (token.type == LZ_TOKEN_IDENTIFIER) {
                    LzLexer peek = ahead;
                    if (lz_lexer_next(&peek).type == LZ_TOKEN_EQUAL) {
                        return true;
                    }
                }
            }
            at_start = token.type == LZ_TOKEN_COMMA;
        }
        if (token.type == LZ_TOKEN_LEFT_PAREN || token.type == LZ_TOKEN_LEFT_BRACKET ||
            token.type == LZ_TOKEN_LEFT_BRACE) {
            depth++;
        } else if (token.type == LZ_TOKEN_RIGHT_PAREN || token.type == LZ_TOKEN_RIGHT_BRACKET ||
                   token.type == LZ_TOKEN_RIGHT_BRACE) {
            if (depth == 0) {
                return false;
            }
            depth--;
        }
        token = lz_lexer_next(&ahead);
    }
}

static const char *sig_name(const FuncSig *sig) {
    return sig->fname != NULL ? sig->fname->data : "<fn>";
}

/* A statically known callee: does the argument fit the parameter it lands in? */
static void check_argument(Parser *parser, const FuncSig *sig, size_t index, const Expr *arg,
                           const LzToken *start) {
    if (sig == NULL) {
        return;
    }
    const LzType *expected;
    const char *name;
    if (index < sig->param_count) {
        expected = &sig->types[index];
        name = sig->names[index]->data;
    } else if (sig->has_rest) {
        expected = &sig->rest_item;
        name = sig->names[sig->param_count]->data;
    } else {
        return;
    }
    if (lz_type_assignable(&arg->type, expected) == LZ_ASSIGN_NEVER) {
        LzToken span = span_from(parser, start);
        char prefix[128];
        (void)snprintf(prefix, sizeof prefix, "type mismatch: argument '%s' of %s() expects ", name,
                       sig_name(sig));
        error_types(parser, &span, prefix, expected, ", got ", &arg->type);
    }
}

static void check_arity(Parser *parser, const FuncSig *sig, size_t given, const LzToken *at) {
    if (sig == NULL || (given >= sig->required && (given <= sig->param_count || sig->has_rest))) {
        return;
    }
    char message[160];
    if (given < sig->required) {
        (void)snprintf(message, sizeof message, "%s() expects %s%u argument%s, got %zu", sig_name(sig),
                       sig->has_rest || sig->required != sig->param_count ? "at least " : "",
                       (unsigned)sig->required, sig->required == 1 ? "" : "s", given);
    } else {
        (void)snprintf(message, sizeof message, "%s() expects %s%u argument%s, got %zu", sig_name(sig),
                       sig->required != sig->param_count ? "at most " : "", (unsigned)sig->param_count,
                       sig->param_count == 1 ? "" : "s", given);
    }
    error_at(parser, at, message);
}

static Expr call_result(Expr callee) {
    if (callee.native != NULL) {
        return expr_of(lz_type_of_kinds(callee.native->result_kinds));
    }
    if (callee.sig != NULL) {
        return expr_of(callee.sig->result);
    }
    return any_expr();
}

/*
 * A call with `*spread` or `name = value` arguments. The positional arguments are
 * collected into one array (the callee spreads it again on entry), and the named
 * values stay on the stack after it:
 *     [callee] [array of positional arguments] [named values...]  OP_CALL_EX
 */
static Expr call_special(Parser *parser, Expr callee, const LzToken *paren, bool receiver) {
    uint32_t offset = paren->offset;
    const FuncSig *sig = callee.sig;
    emit_op_at(parser, LZ_OP_ARRAY_NEW, offset);
    if (receiver) {
        /* [callee][receiver][array] -> [callee][array][receiver] -> [callee][array] */
        emit_op_at(parser, LZ_OP_SWAP, offset);
        emit_op_at(parser, LZ_OP_ARRAY_PUSH, offset);
    }

    LzBuffer names = {0};
    LzWriter names_out = lz_writer_from_buffer(&names);
    LzString *seen[LZ_MAX_ARGUMENTS];
    size_t named = 0;
    size_t positional = receiver ? 1 : 0;
    bool spread = false;

    if (!check(parser, LZ_TOKEN_RIGHT_PAREN)) {
        do {
            if (check(parser, LZ_TOKEN_RIGHT_PAREN)) {
                break;
            }
            if (match(parser, LZ_TOKEN_STAR)) {
                if (named > 0) {
                    error_at_previous(parser, "a positional argument cannot follow a named one");
                }
                LzToken start = parser->current;
                Expr list = expression(parser);
                if ((lz_type_possible_kinds(&list.type) & LZ_KIND_ARRAY) == 0) {
                    LzToken span = span_from(parser, &start);
                    error_types(parser, &span, "cannot spread a value of type ", &list.type, "", NULL);
                }
                emit_op_at(parser, LZ_OP_ARRAY_EXTEND, offset);
                spread = true;
            } else if (check(parser, LZ_TOKEN_IDENTIFIER) && next_token_type(parser) == LZ_TOKEN_EQUAL) {
                advance(parser);
                LzToken name_token = parser->previous;
                LzString *name = intern_token(parser, &name_token);
                advance(parser); /* the '=' */
                skip_newlines(parser);
                for (size_t i = 0; name != NULL && i < named; i++) {
                    if (seen[i] == name) {
                        error_with_name(parser, &name_token, "argument '", "' is given twice");
                    }
                }
                if (callee.native != NULL) {
                    error_at(parser, &name_token, "builtin functions do not take named arguments");
                } else if (sig != NULL && name != NULL) {
                    size_t index = 0;
                    while (index < sig->param_count && sig->names[index] != name) {
                        index++;
                    }
                    if (index == sig->param_count) {
                        LzSuggestions close = suggest_parameters(parser, &name_token, sig);
                        error_with_name_suggest(parser, &name_token, "unexpected named argument '",
                                                "'", "the function has no parameter with this name",
                                                &close);
                    } else if (index < positional && !spread) {
                        error_with_name(parser, &name_token, "argument '", "' is already given by position");
                    }
                }
                LzToken start = parser->current;
                Expr value = expression(parser);
                if (sig != NULL && name != NULL) {
                    for (size_t index = 0; index < sig->param_count; index++) {
                        if (sig->names[index] == name) {
                            check_argument(parser, sig, index, &value, &start);
                        }
                    }
                }
                if (named >= LZ_MAX_ARGUMENTS) {
                    error_at_previous(parser, "too many named arguments");
                } else if (name != NULL) {
                    seen[named] = name;
                    if (named > 0) {
                        (void)lz_writer_write(&names_out, "\0", 1);
                    }
                    (void)lz_writer_write(&names_out, name->data, name->length);
                    named++;
                }
            } else {
                if (named > 0) {
                    error_at_current(parser, "a positional argument cannot follow a named one");
                }
                LzToken start = parser->current;
                Expr value = expression(parser);
                check_argument(parser, sig, positional, &value, &start);
                emit_op_at(parser, LZ_OP_ARRAY_PUSH, offset);
                positional++;
            }
        } while (match(parser, LZ_TOKEN_COMMA));
    }
    consume(parser, LZ_TOKEN_RIGHT_PAREN, "expected ')' after arguments");

    if (sig != NULL && !spread) {
        check_arity(parser, sig, positional + named, paren);
        for (size_t i = positional; i < sig->required; i++) {
            bool given = false;
            for (size_t k = 0; k < named; k++) {
                given = given || seen[k] == sig->names[i];
            }
            if (!given && positional + named >= sig->required) {
                char message[96];
                (void)snprintf(message, sizeof message, "%s() is missing argument '%s'", sig_name(sig),
                               sig->names[i]->data);
                error_at(parser, paren, message);
                break;
            }
        }
    }

    uint16_t names_index = 0;
    if (named > 0) {
        LzString *text = lz_string_new(&parser->vm->heap, names.data, names.length);
        if (text == NULL) {
            fail(parser, lz_vm_out_of_memory(parser->vm));
        } else {
            names_index = make_constant(parser, lz_value_object(&text->base));
            lz_object_release(&parser->vm->heap, &text->base);
        }
    }
    lz_buffer_free(&names);

    emit_op_at(parser, LZ_OP_CALL_EX, offset);
    emit_byte_at(parser, (uint8_t)named, offset);
    emit_u16_at(parser, names_index, offset);
    return call_result(callee);
}

/*
 * receiver: a method call, `receiver.name(args)`. The receiver is already on the stack
 * with the callee on top of it (see dot_call), so it counts as the first argument.
 */
static Expr call_args(Parser *parser, Expr callee, bool receiver) {
    LzToken paren = parser->previous;
    uint32_t offset = paren.offset;
    unsigned count = receiver ? 1 : 0;

    if ((lz_type_possible_kinds(&callee.type) & LZ_KIND_FUNCTION) == 0) {
        error_types(parser, &paren, "cannot call a value of type ", &callee.type, "", NULL);
    }
    if (call_has_special_args(parser)) {
        return call_special(parser, callee, &paren, receiver);
    }

    if (!check(parser, LZ_TOKEN_RIGHT_PAREN)) {
        do {
            if (check(parser, LZ_TOKEN_RIGHT_PAREN)) {
                break;
            }
            LzToken start = parser->current;
            Expr arg = expression(parser);
            check_argument(parser, callee.sig, count, &arg, &start);
            if (++count > LZ_MAX_ARGUMENTS) {
                error_at_previous(parser, "too many arguments");
                count = LZ_MAX_ARGUMENTS;
            }
        } while (match(parser, LZ_TOKEN_COMMA));
    }
    consume(parser, LZ_TOKEN_RIGHT_PAREN, "expected ')' after arguments");
    check_arity(parser, callee.sig, count, &paren);

    emit_op_at(parser, LZ_OP_CALL, offset);
    emit_byte_at(parser, (uint8_t)count, offset);
    return call_result(callee);
}

static Expr call(Parser *parser, Expr callee) {
    return call_args(parser, callee, false);
}

/*
 * receiver.name(args) is name(receiver, args): the callee is looked up like any other
 * name (locals, functions of the script, builtins), pushed above the receiver, and the
 * two are swapped so the call finds [callee][receiver][args...].
 */
static Expr dot_call(Parser *parser, Expr receiver) {
    (void)receiver; /* its type will pick the method table once types have their own methods */
    if (!check(parser, LZ_TOKEN_IDENTIFIER)) {
        error_at_current(parser, "expected a method name after '.'");
        return any_expr();
    }
    advance(parser);
    LzToken name = parser->previous;
    if (!check(parser, LZ_TOKEN_LEFT_PAREN)) {
        error_at(parser, &name, "expected '(' after the method name (fields are not supported yet)");
        return any_expr();
    }

    /*
     * Your own functions and variables of that name win; then the methods of the kinds of
     * value (one dispatcher per name, see builtin/method); then the other builtins.
     */
    LzString *interned = intern_token(parser, &name);
    const LzValue *method = NULL;
    if (interned != NULL && find_symbol(parser, interned) == NULL &&
        lz_table_get(&parser->vm->globals, interned) == NULL) {
        method = lz_table_get(&parser->vm->methods, interned);
    }
    Expr callee;
    if (method != NULL) {
        (void)emit_constant(parser, *method);
        callee = expr_of(lz_type_of_kinds(LZ_KIND_FUNCTION));
        callee.native = (const LzNative *)method->as.object;
    } else {
        parser->method_name = true;
        callee = variable(parser);
        parser->method_name = false;
    }
    emit_op_at(parser, LZ_OP_SWAP, name.offset);
    advance(parser); /* the '(' */
    return call_args(parser, callee, true);
}

static Expr and_(Parser *parser, Expr left) {
    uint32_t offset = parser->previous.offset;
    size_t end_jump = emit_jump(parser, LZ_OP_JUMP_IF_FALSE, offset);
    emit_op_at(parser, LZ_OP_POP, offset);
    skip_newlines(parser);
    /* One level up: the caller's loop absorbs the next "and", so a long chain iterates. */
    Expr right = parse_precedence(parser, PREC_EQUALITY);
    patch_jump(parser, end_jump);
    return union_expr(left, right);
}

static Expr or_(Parser *parser, Expr left) {
    uint32_t offset = parser->previous.offset;
    size_t end_jump = emit_jump(parser, LZ_OP_JUMP_IF_TRUE, offset);
    emit_op_at(parser, LZ_OP_POP, offset);
    skip_newlines(parser);
    Expr right = parse_precedence(parser, PREC_AND);
    patch_jump(parser, end_jump);
    return union_expr(left, right);
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
    LzChunk temp = {.code = code,
                    .offsets = offsets,
                    .count = size + 1,
                    .capacity = size + 1,
                    .constants = chunk->constants,
                    .constant_count = chunk->constant_count,
                    .constant_capacity = chunk->constant_count};

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
                             bool is_const, const LzType *type) {
    if (parser->scope_depth == 0) {
        Symbol *symbol = add_symbol(parser, name_token, name, SYMBOL_GLOBAL, is_const,
                                    lz_value_nil(), type);
        if (symbol != NULL) {
            emit_named(parser, LZ_OP_DEFINE_GLOBAL, name, name_token->offset);
        }
    } else {
        add_symbol(parser, name_token, name, SYMBOL_LOCAL, is_const, lz_value_nil(), type);
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

    LzType declared = lz_type_of_kinds(LZ_KINDS_ANY);
    bool annotated = false;
    if (match(parser, LZ_TOKEN_COLON)) {
        annotated = true;
        (void)parse_type_union(parser, &declared);
    }

    size_t code_mark = parser->chunk->count;
    size_t constant_mark = parser->chunk->constant_count;
    LzToken initializer_start = parser->current;
    bool has_initializer = false;
    Expr initializer = expr_of(lz_type_of_kinds(LZ_KIND_NIL));

    if (match(parser, LZ_TOKEN_EQUAL)) {
        skip_newlines(parser);
        initializer_start = parser->current;
        initializer = expression(parser);
        has_initializer = true;
    } else if (is_const) {
        error_at_current(parser, "expected '=' after constant name");
        return;
    } else {
        emit_op_at(parser, LZ_OP_NIL, name_token.offset);
        if (annotated && lz_type_assignable(&initializer.type, &declared) == LZ_ASSIGN_NEVER) {
            error_types(parser, &name_token, "variable of type ", &declared,
                        " needs an initial value", NULL);
        }
    }
    end_statement(parser);

    if (is_const && has_initializer && !parser->panic && parser->fatal == LZ_OK &&
        parser->chunk->count > code_mark && fragment_is_pure(parser->chunk, code_mark)) {
        LzValue value = lz_value_nil();
        LzStatus status = evaluate_fragment(parser, code_mark, &value);
        if (status == LZ_OK) {
            forget_constants_from(parser, constant_mark);
            lz_chunk_truncate(&parser->vm->heap, parser->chunk, code_mark, constant_mark);

            LzType value_type = lz_type_of_value(&value);
            if (annotated && lz_type_assignable(&value_type, &declared) == LZ_ASSIGN_NEVER) {
                LzToken span = span_from(parser, &initializer_start);
                error_types(parser, &span, "type mismatch: expected ", &declared, ", got ",
                            &value_type);
            }
            if (add_symbol(parser, &name_token, name, SYMBOL_CONSTANT, true, value,
                           &value_type) == NULL) {
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

    if (annotated && has_initializer) {
        check_assignment(parser, &initializer.type, &declared, &initializer_start,
                         name_token.offset);
    }
    LzType stored = declared;
    if (!annotated && is_const) {
        stored = lz_type_widen(&initializer.type);
    }
    declare_variable(parser, &name_token, name, is_const, &stored);
}

static void assignment_statement(Parser *parser) {
    advance(parser);
    LzToken name_token = parser->previous;
    LzString *name = intern_token(parser, &name_token);
    advance(parser);
    skip_newlines(parser);
    LzToken value_start = parser->current;
    Expr value = expression(parser);
    if (name == NULL) {
        return;
    }

    Symbol *symbol = find_symbol(parser, name);
    if (symbol == NULL) {
        if (lz_vm_lookup(parser->vm, name) != NULL) {
            error_with_name(parser, &name_token, "cannot assign to builtin '", "'");
        } else {
            /* Sequential execution: checked when the assignment runs (SET_GLOBAL). */
            record_name_hint(parser, &name_token, false, true, false);
            emit_named(parser, LZ_OP_SET_GLOBAL, name, name_token.offset);
        }
    } else if (symbol->is_const || symbol->kind == SYMBOL_CONSTANT) {
        error_with_name(parser, &name_token, "cannot assign to constant '", "'");
    } else {
        check_assignment(parser, &value.type, &symbol->type, &value_start, name_token.offset);
        if (symbol->kind == SYMBOL_LOCAL && symbol->owner != parser->fs) {
            int index = resolve_upvalue(parser, parser->fs, symbol);
            emit_op_at(parser, LZ_OP_SET_UPVALUE, name_token.offset);
            emit_u16_at(parser, (uint16_t)(index < 0 ? 0 : index), name_token.offset);
        } else if (symbol->kind == SYMBOL_LOCAL) {
            emit_op_at(parser, LZ_OP_SET_LOCAL, name_token.offset);
            emit_u16_at(parser, symbol->slot, name_token.offset);
        } else {
            emit_named(parser, LZ_OP_SET_GLOBAL, name, name_token.offset);
        }
    }
    end_statement(parser);
}

static bool is_type_keyword(const Parser *parser) {
    return parser->current.length == 4 &&
           memcmp(parser->source->text + parser->current.offset, "type", 4) == 0;
}

/* type Name = <type>; "type" is only a keyword when a name follows it. */
static void type_declaration(Parser *parser) {
    advance(parser);
    LzToken name_token = parser->previous;
    LzString *name = intern_token(parser, &name_token);
    if (name == NULL) {
        return;
    }

    char text[48];
    LzType ignored;
    token_text(parser, &name_token, text, sizeof text);
    if (builtin_type(text, &ignored)) {
        error_with_name(parser, &name_token, "cannot redefine builtin type '", "'");
        return;
    }

    consume(parser, LZ_TOKEN_EQUAL, "expected '=' after type name");
    skip_newlines(parser);
    LzType type;
    (void)parse_type_union(parser, &type);
    end_statement(parser);

    for (size_t i = parser->alias_count; i > 0; i--) {
        const Alias *existing = &parser->aliases[i - 1];
        if (existing->depth < parser->scope_depth) {
            break;
        }
        if (existing->name == name) {
            error_with_name(parser, &name_token, "type '", "' is already declared in this scope");
            return;
        }
    }
    if (parser->alias_count >= LZ_MAX_ALIASES) {
        error_at(parser, &name_token, "too many type aliases");
        return;
    }
    Alias *alias = &parser->aliases[parser->alias_count++];
    alias->name = name;
    alias->depth = parser->scope_depth;
    alias->type = type;
}

static void block(Parser *parser) {
    if (parser->nesting >= LZ_MAX_NESTING) {
        error_at_current(parser, "blocks nested too deeply");
        return;
    }
    parser->nesting++;
    consume(parser, LZ_TOKEN_LEFT_BRACE, "expected '{'");
    begin_scope(parser);
    bool returns = false;
    while (!check(parser, LZ_TOKEN_RIGHT_BRACE) && !check(parser, LZ_TOKEN_EOF) &&
           parser->fatal == LZ_OK) {
        declaration(parser);
        returns = returns || parser->terminated;
    }
    consume(parser, LZ_TOKEN_RIGHT_BRACE, "expected '}' after block");
    end_scope(parser);
    parser->nesting--;
    parser->terminated = returns;
}

/*
 * An else-if chain is a loop, not recursion: a script must not be able to
 * overflow the C stack with a long chain. Every branch ends with a jump to the
 * end of the whole statement, collected here and patched once the chain is done.
 */
static void if_statement(Parser *parser) {
    size_t *end_jumps = NULL;
    size_t end_count = 0;
    size_t end_capacity = 0;
    bool all_return = true; /* every branch ends in a return, and there is an else */
    bool has_else = false;

    for (;;) {
        /* previous is this branch's "if", so each branch's jumps point at their own line. */
        uint32_t offset = parser->previous.offset;
        (void)expression(parser);
        if (!check(parser, LZ_TOKEN_LEFT_BRACE)) {
            error_at_current(parser, "expected '{' after condition");
            break;
        }

        size_t then_jump = emit_jump(parser, LZ_OP_JUMP_IF_FALSE, offset);
        emit_op_at(parser, LZ_OP_POP, offset);
        block(parser);
        all_return = all_return && parser->terminated;

        if (end_count == end_capacity) {
            size_t capacity = end_capacity != 0 ? end_capacity * 2 : 4;
            size_t *grown = realloc(end_jumps, capacity * sizeof *grown);
            if (grown == NULL) {
                fail(parser, LZ_ERR_MEMORY);
                break;
            }
            end_jumps = grown;
            end_capacity = capacity;
        }
        end_jumps[end_count++] = emit_jump(parser, LZ_OP_JUMP, offset);
        patch_jump(parser, then_jump);
        emit_op_at(parser, LZ_OP_POP, offset);

        skip_newlines(parser);
        if (!match(parser, LZ_TOKEN_ELSE)) {
            break;
        }
        if (match(parser, LZ_TOKEN_IF)) {
            continue;
        }
        if (check(parser, LZ_TOKEN_LEFT_BRACE)) {
            block(parser);
            has_else = true;
            all_return = all_return && parser->terminated;
        } else {
            error_at_current(parser, "expected '{' or 'if' after 'else'");
        }
        break;
    }

    for (size_t i = 0; i < end_count; i++) {
        patch_jump(parser, end_jumps[i]);
    }
    free(end_jumps);
    parser->terminated = has_else && all_return;
}

static void while_statement(Parser *parser) {
    uint32_t offset = parser->previous.offset;
    size_t loop_start = parser->chunk->count;
    /* `while true { ... }` has no way out but a return, so nothing after it is reachable. */
    bool forever = check(parser, LZ_TOKEN_TRUE) && next_token_type(parser) == LZ_TOKEN_LEFT_BRACE;
    (void)expression(parser);
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
    parser->terminated = forever;
}

static LzTokenType next_token_type(const Parser *parser) {
    LzLexer ahead = parser->lexer;
    return lz_lexer_next(&ahead).type;
}

static void expression_statement(Parser *parser) {
    uint32_t offset = parser->current.offset;
    Expr target = expression(parser);

    /* a[i] = v: the expression just compiled ends in OP_INDEX, which becomes OP_SET_INDEX. */
    if (check(parser, LZ_TOKEN_EQUAL) && target.is_index && parser->fatal == LZ_OK &&
        parser->chunk->count == target.op_pos + 1) {
        uint32_t index_offset = parser->chunk->offsets[target.op_pos];
        lz_chunk_truncate(&parser->vm->heap, parser->chunk, target.op_pos,
                          parser->chunk->constant_count);
        advance(parser);
        skip_newlines(parser);
        LzToken value_start = parser->current;
        Expr value = expression(parser);
        if (lz_type_assignable(&value.type, &target.type) == LZ_ASSIGN_NEVER) {
            LzToken span = span_from(parser, &value_start);
            error_types(parser, &span, "type mismatch: expected ", &target.type, ", got ",
                        &value.type);
        }
        emit_op_at(parser, LZ_OP_SET_INDEX, index_offset);
        end_statement(parser);
        return;
    }

    emit_op_at(parser, LZ_OP_POP, offset);
    end_statement(parser);
}

/* Functions */

static FuncSig *new_sig(Parser *parser, LzString *name) {
    FuncSig *sig = calloc(1, sizeof *sig);
    if (sig == NULL) {
        fail(parser, LZ_ERR_MEMORY);
        return NULL;
    }
    sig->next = parser->sigs;
    parser->sigs = sig;
    sig->fname = name;
    sig->result = lz_type_of_kinds(LZ_KINDS_ANY);
    sig->rest_item = lz_type_of_kinds(LZ_KINDS_ANY);
    for (size_t i = 0; i < LZ_MAX_PARAMS; i++) {
        sig->types[i] = lz_type_of_kinds(LZ_KINDS_ANY);
    }
    return sig;
}

typedef struct ParamCheck {
    uint16_t slot;
    LzType type;
} ParamCheck;

/*
 * Compiles `( params ) [-> type] { body }` into a new function and emits the code that
 * creates it in the enclosing function. `self` is the symbol of a `fn name` (so the
 * body can call itself and its signature can be attached), or SIZE_MAX.
 */
static Expr compile_function(Parser *parser, LzString *name, size_t self, uint32_t offset) {
    Expr result = expr_of(lz_type_of_kinds(LZ_KIND_FUNCTION));
    LzHeap *heap = &parser->vm->heap;
    if (parser->nesting >= LZ_MAX_NESTING) {
        error_at_current(parser, "functions nested too deeply");
        return result;
    }
    LzFunction *function = lz_function_new(heap, name);
    FuncSig *sig = function != NULL ? new_sig(parser, name) : NULL;
    if (function == NULL || sig == NULL) {
        if (function != NULL) {
            lz_object_release(heap, &function->base);
        } else {
            fail(parser, lz_vm_out_of_memory(parser->vm));
        }
        return result;
    }
    function->offset = offset;

    FuncState fs;
    fs.enclosing = parser->fs;
    fs.function = function;
    fs.first_symbol = parser->symbol_count;
    fs.self_symbol = self;
    fs.sig = sig;
    fs.upvalue_count = 0;
    fs.saved_chunk = parser->chunk;
    fs.saved_constants = parser->constants;
    fs.saved_local_count = parser->local_count;
    fs.saved_terminated = parser->terminated;
    parser->chunk = &function->chunk;
    memset(&parser->constants, 0, sizeof parser->constants);
    parser->local_count = 0;
    parser->fs = &fs;
    parser->nesting++;
    begin_scope(parser);

    /* Parameters */
    LzString *names[LZ_MAX_PARAMS + 1];
    ParamCheck checks[LZ_MAX_PARAMS + 1];
    size_t check_count = 0;
    size_t param_count = 0;
    size_t required = 0;
    bool has_rest = false;
    bool seen_default = false;

    consume(parser, LZ_TOKEN_LEFT_PAREN, "expected '(' after 'fn'");
    if (!check(parser, LZ_TOKEN_RIGHT_PAREN) && !parser->panic) {
        do {
            if (check(parser, LZ_TOKEN_RIGHT_PAREN)) {
                break; /* trailing comma */
            }
            if (has_rest) {
                error_at_current(parser, "the rest parameter must be the last one");
                break;
            }
            bool rest = match(parser, LZ_TOKEN_STAR);
            if (!check(parser, LZ_TOKEN_IDENTIFIER)) {
                error_at_current(parser, "expected a parameter name");
                break;
            }
            advance(parser);
            LzToken param_token = parser->previous;
            LzString *param = intern_token(parser, &param_token);
            if (param == NULL) {
                break;
            }
            LzType declared = lz_type_of_kinds(LZ_KINDS_ANY);
            if (match(parser, LZ_TOKEN_COLON)) {
                (void)parse_type_union(parser, &declared);
            }
            uint16_t slot = (uint16_t)parser->local_count;

            if (rest) {
                if (check(parser, LZ_TOKEN_EQUAL)) {
                    error_at_current(parser, "a rest parameter cannot have a default");
                }
                LzType list = array_type(parser, &declared);
                (void)add_symbol(parser, &param_token, param, SYMBOL_LOCAL, false, lz_value_nil(), &list);
                names[param_count] = param;
                sig->names[param_count] = param;
                sig->rest_item = declared;
                has_rest = true;
                checks[check_count++] = (ParamCheck){slot, list};
                continue;
            }

            if (param_count >= LZ_MAX_PARAMS) {
                error_at(parser, &param_token, "too many parameters (limit 64)");
                break;
            }
            if (match(parser, LZ_TOKEN_EQUAL)) {
                skip_newlines(parser);
                seen_default = true;
                /* Runs only when the caller left this argument out. */
                emit_op_at(parser, LZ_OP_JUMP_IF_SUPPLIED, param_token.offset);
                emit_u16_at(parser, slot, param_token.offset);
                emit_u16_at(parser, 0xFFFF, param_token.offset);
                size_t jump = parser->chunk->count - 2;
                LzToken value_start = parser->current;
                Expr value = expression(parser);
                if (lz_type_assignable(&value.type, &declared) == LZ_ASSIGN_NEVER) {
                    LzToken span = span_from(parser, &value_start);
                    error_types(parser, &span, "type mismatch: expected ", &declared, ", got ",
                                &value.type);
                }
                emit_op_at(parser, LZ_OP_SET_LOCAL, param_token.offset);
                emit_u16_at(parser, slot, param_token.offset);
                patch_jump(parser, jump);
            } else if (seen_default) {
                error_at(parser, &param_token, "a parameter without a default cannot follow one with a default");
            } else {
                required = param_count + 1;
            }
            (void)add_symbol(parser, &param_token, param, SYMBOL_LOCAL, false, lz_value_nil(), &declared);
            names[param_count] = param;
            sig->names[param_count] = param;
            sig->types[param_count] = declared;
            if (!is_any_type(&declared)) {
                checks[check_count++] = (ParamCheck){slot, declared};
            }
            param_count++;
        } while (match(parser, LZ_TOKEN_COMMA));
    }
    consume(parser, LZ_TOKEN_RIGHT_PAREN, "expected ')' after parameters");
    if (!parser->panic && match(parser, LZ_TOKEN_ARROW)) {
        sig->result_declared = true;
        (void)parse_type_union(parser, &sig->result);
    }
    sig->param_count = (uint16_t)param_count;
    sig->required = (uint16_t)required;
    sig->has_rest = has_rest;
    if (self != SIZE_MAX) {
        parser->symbols[self].sig = sig;
    }

    /* Argument types are checked once, on entry, after the defaults are in place. */
    for (size_t i = 0; i < check_count; i++) {
        uint16_t type_index = 0;
        if (!lz_chunk_add_type(parser->chunk, &checks[i].type, &type_index)) {
            error_at_previous(parser, "too many types in one function");
            break;
        }
        emit_op_at(parser, LZ_OP_CHECK_PARAM, offset);
        emit_u16_at(parser, checks[i].slot, offset);
        emit_u16_at(parser, type_index, offset);
    }

    /* Body: same scope as the parameters, so `let` cannot silently shadow one. */
    bool returns = false;
    bool block_after_arrow = false;
    if (check(parser, LZ_TOKEN_FAT_ARROW)) {
        /* fn(x) => expression: the same as fn(x) { return expression } */
        advance(parser);
        uint32_t arrow = parser->previous.offset;
        skip_newlines(parser);
        if (check(parser, LZ_TOKEN_LEFT_BRACE)) {
            /* `=>` takes one expression; a block is the plain form. */
            error_at_current(parser, "a block body is written without '=>': fn(x) { ... }");
            block_after_arrow = true; /* still read as the body, so one mistake is one error */
        } else {
            LzToken start = parser->current;
            Expr value = expression(parser);
            if (sig->result_declared) {
                check_flow(parser, &value.type, &sig->result, &start, arrow, LZ_OP_CHECK_RETURN,
                           "return type mismatch: expected ");
            }
            emit_op_at(parser, LZ_OP_RETURN, arrow);
            returns = true;
        }
    }
    if (block_after_arrow || (!returns && check(parser, LZ_TOKEN_LEFT_BRACE))) {
        advance(parser);
        while (!check(parser, LZ_TOKEN_RIGHT_BRACE) && !check(parser, LZ_TOKEN_EOF) &&
               parser->fatal == LZ_OK) {
            declaration(parser);
            returns = returns || parser->terminated;
        }
        consume(parser, LZ_TOKEN_RIGHT_BRACE, "expected '}' after function body");
    } else if (!returns) {
        error_at_current(parser, "expected '{' or '=>' before function body");
    }

    LzToken close = parser->previous;
    if (!returns && sig->result_declared) {
        LzType nothing = lz_type_of_kinds(LZ_KIND_NIL);
        if (lz_type_assignable(&nothing, &sig->result) == LZ_ASSIGN_NEVER) {
            error_types(parser, &close, "missing return: the function must return a value of type ",
                        &sig->result, "", NULL);
        }
    }
    emit_op_at(parser, LZ_OP_NIL, close.offset);
    emit_op_at(parser, LZ_OP_RETURN, close.offset);

    /* Leave the function. */
    while (parser->symbol_count > fs.first_symbol) {
        Symbol *symbol = &parser->symbols[--parser->symbol_count];
        if (symbol->kind == SYMBOL_CONSTANT) {
            lz_value_release(heap, &symbol->value);
        }
    }
    while (parser->alias_count > 0 && parser->aliases[parser->alias_count - 1].depth == parser->scope_depth) {
        parser->alias_count--;
    }
    parser->scope_depth--;
    parser->nesting--;

    if (!lz_function_set_params(function, names, param_count, required, has_rest) ||
        !lz_function_set_upvalues(function, fs.upvalues, fs.upvalue_count)) {
        fail(parser, LZ_ERR_MEMORY);
    }
    free(parser->constants.slots);
    parser->chunk = fs.saved_chunk;
    parser->constants = fs.saved_constants;
    parser->local_count = fs.saved_local_count;
    parser->fs = fs.enclosing;
    parser->terminated = false;
    (void)fs.saved_terminated;

    /* In the enclosing function: build the value. A function that captures nothing is made once. */
    if (fs.upvalue_count == 0) {
        LzClosure *closure = lz_closure_new(heap, function);
        if (closure == NULL) {
            fail(parser, lz_vm_out_of_memory(parser->vm));
        } else {
            uint16_t index = add_constant(parser, lz_value_object(&closure->base), true);
            emit_op_at(parser, LZ_OP_CONSTANT, offset);
            emit_u16_at(parser, index, offset);
            lz_object_release(heap, &closure->base);
        }
    } else {
        uint16_t index = add_constant(parser, lz_value_object(&function->base), true);
        emit_op_at(parser, LZ_OP_CLOSURE, offset);
        emit_u16_at(parser, index, offset);
    }
    lz_object_release(heap, &function->base);

    result.sig = sig;
    return result;
}

/* fn(params) { ... } as an expression. */
static Expr function_expression(Parser *parser) {
    return compile_function(parser, NULL, SIZE_MAX, parser->previous.offset);
}

/* fn name(params) { ... }: a constant binding, global at the top level and a local elsewhere. */
static void function_declaration(Parser *parser) {
    advance(parser); /* the name */
    LzToken name_token = parser->previous;
    LzString *name = intern_token(parser, &name_token);
    if (name == NULL) {
        return;
    }

    LzType type = lz_type_of_kinds(LZ_KIND_FUNCTION);
    size_t self = SIZE_MAX;
    if (parser->scope_depth == 0) {
        Symbol *existing = find_symbol(parser, name);
        if (existing != NULL && existing->pending) {
            existing->pending = false; /* declared ahead of time by prescan_functions */
            self = (size_t)(existing - parser->symbols);
        } else {
            Symbol *symbol = add_symbol(parser, &name_token, name, SYMBOL_GLOBAL, true, lz_value_nil(), &type);
            if (symbol != NULL) {
                self = (size_t)(symbol - parser->symbols);
            }
        }
    } else {
        Symbol *symbol = add_symbol(parser, &name_token, name, SYMBOL_LOCAL, true, lz_value_nil(), &type);
        if (symbol != NULL) {
            self = (size_t)(symbol - parser->symbols);
        }
    }

    (void)compile_function(parser, name, self, name_token.offset);
    if (self != SIZE_MAX && parser->scope_depth == 0) {
        emit_named(parser, LZ_OP_DEFINE_GLOBAL, name, name_token.offset);
    }
    parser->terminated = false;
}

static void return_statement(Parser *parser) {
    uint32_t offset = parser->previous.offset;
    const FuncState *fs = parser->fs;
    if (fs->function == NULL) {
        error_at_previous(parser, "'return' outside of a function");
    }

    LzToken start = parser->current;
    Expr value = expr_of(lz_type_of_kinds(LZ_KIND_NIL));
    if (check(parser, LZ_TOKEN_NEWLINE) || check(parser, LZ_TOKEN_SEMICOLON) ||
        check(parser, LZ_TOKEN_RIGHT_BRACE) || check(parser, LZ_TOKEN_EOF)) {
        emit_op_at(parser, LZ_OP_NIL, offset);
    } else {
        value = expression(parser);
    }
    if (fs->function != NULL && fs->sig->result_declared) {
        check_flow(parser, &value.type, &fs->sig->result, &start, offset, LZ_OP_CHECK_RETURN,
                   "return type mismatch: expected ");
    }
    emit_op_at(parser, LZ_OP_RETURN, offset);
    end_statement(parser);
    parser->terminated = true;
}

/*
 * Top-level `fn` declarations are visible to the whole file, so functions can call
 * each other whatever their order. Only the names are declared here; the function
 * itself exists once its declaration has run.
 */
static void prescan_functions(Parser *parser) {
    LzLexer lexer;
    lz_lexer_init(&lexer, parser->source);
    int depth = 0;
    LzType type = lz_type_of_kinds(LZ_KIND_FUNCTION);
    for (;;) {
        LzToken token = lz_lexer_next(&lexer);
        if (token.type == LZ_TOKEN_EOF) {
            break;
        }
        if (token.type == LZ_TOKEN_LEFT_BRACE) {
            depth++;
        } else if (token.type == LZ_TOKEN_RIGHT_BRACE) {
            if (depth > 0) {
                depth--;
            }
        } else if (token.type == LZ_TOKEN_FN && depth == 0) {
            LzLexer peek = lexer;
            LzToken name_token = lz_lexer_next(&peek);
            if (name_token.type != LZ_TOKEN_IDENTIFIER || parser->symbol_count >= LZ_MAX_SYMBOLS) {
                continue;
            }
            LzString *name = intern_token(parser, &name_token);
            if (name == NULL) {
                return;
            }
            if (find_symbol(parser, name) == NULL) {
                Symbol *symbol = add_symbol(parser, &name_token, name, SYMBOL_GLOBAL, true,
                                            lz_value_nil(), &type);
                if (symbol != NULL) {
                    symbol->pending = true;
                }
            }
        }
    }
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
        case LZ_TOKEN_FN:
        case LZ_TOKEN_RETURN:
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
    parser->terminated = false;

    switch (parser->current.type) {
    case LZ_TOKEN_FN:
        if (next_token_type(parser) == LZ_TOKEN_IDENTIFIER) {
            advance(parser);
            function_declaration(parser);
        } else {
            expression_statement(parser);
        }
        break;
    case LZ_TOKEN_RETURN:
        advance(parser);
        return_statement(parser);
        break;
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
        if (is_type_keyword(parser) && next_token_type(parser) == LZ_TOKEN_IDENTIFIER) {
            advance(parser);
            type_declaration(parser);
        } else if (next_token_type(parser) == LZ_TOKEN_EQUAL) {
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
    FuncState script;
    memset(&script, 0, sizeof script);
    script.self_symbol = SIZE_MAX;
    parser->fs = &script;
    prescan_functions(parser);
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
    diagnostics->hint_count = 0;

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
    while (parser->sigs != NULL) {
        FuncSig *next = parser->sigs->next;
        free(parser->sigs);
        parser->sigs = next;
    }
    free(parser->constants.slots);
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

const LzNameHint *lz_diagnostics_find_hint(const LzDiagnostics *diagnostics, uint32_t offset) {
    for (size_t i = 0; i < diagnostics->hint_count; i++) {
        if (diagnostics->hints[i].offset == offset) {
            return &diagnostics->hints[i];
        }
    }
    return NULL;
}

void lz_diagnostics_report(const LzDiagnostics *diagnostics, const LzSource *source, bool color,
                           const LzWriter *out) {
    for (size_t i = 0; i < diagnostics->count; i++) {
        const LzDiagnostic *item = &diagnostics->items[i];
        LzReport report = {0};
        report.severity = "error";
        report.message = item->message;
        report.offset = item->offset;
        report.length = item->length;
        report.label = item->label;
        report.suggestions = &item->suggestions;
        lz_report_write(source, &report, color, out);
    }
    lz_report_write_summary(diagnostics->count + diagnostics->dropped, diagnostics->dropped, color,
                            out);
}
