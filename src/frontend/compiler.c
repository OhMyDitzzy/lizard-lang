#include "lizard/compiler.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lizard/array.h"
#include "lizard/class.h"
#include "lizard/dict.h"
#include "lizard/format.h"
#include "lizard/function.h"
#include "lizard/lexer.h"
#include "lizard/native.h"
#include "lizard/report.h"
#include "lizard/strlit.h"
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
    bool loop_var;           /* a `for ... in` variable: it cannot be assigned */
    bool catch_var;          /* the variable of a `catch` block: it cannot be assigned */
    bool dict_value;         /* initialised from a dict literal: `x[k]` reads a key */
    bool array_value;        /* initialised from an array: `x[k]` needs an int index */
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
    bool static_member;   /* a function without `self` in a class body: `self` is not available */
    bool method_member;   /* a method or property of a class (it has `self`): `super` is available */
} FuncState;

typedef struct LoopFrame LoopFrame;

/*
 * Where a `finally` body starts, to compile it again at every way out of its `try`.
 * The lexer is positioned after `current`, which is the `{` of the body.
 */
typedef struct FinallySnapshot {
    LzLexer lexer;
    LzToken current;
    LzToken previous;
} FinallySnapshot;

/*
 * A region guarded by a handler (an open OP_TRY_BEGIN) that `return`, `break` and `continue`
 * leave: each must close it, and run the `finally` body of a try that has one.
 */
typedef struct TryContext {
    struct TryContext *enclosing;
    FuncState *func;     /* the function the try is in */
    LoopFrame *loop;     /* the innermost loop around it */
    bool has_finally;
    bool failed;         /* a copy of the finally body reported errors: no more copies */
    size_t symbol_start; /* symbols from here on were declared inside the try */
    FinallySnapshot snapshot;
} TryContext;

/* A finally body being compiled: it may not be left with return, break or continue. */
typedef struct FinallyGuard {
    bool active;
    FuncState *func;
    LoopFrame *loop;
} FinallyGuard;

/*
 * An `error Name : Parent` declaration found by the prescan, which declares every one of them
 * in the VM before the first statement is compiled (see prescan_errors). The mistakes it met are
 * kept here and told when the parser reaches the declaration, so they come in source order.
 */
typedef enum ErrorProblem {
    ERROR_DECL_OK,
    ERROR_DECL_DUPLICATE,      /* a kind of that name exists */
    ERROR_DECL_COLLISION,      /* a variable or function has the name */
    ERROR_DECL_UNKNOWN_PARENT, /* the parent is not a kind, or is declared further down */
    ERROR_DECL_LIMIT           /* too many kinds */
} ErrorProblem;

typedef struct ErrorDecl {
    uint32_t name_offset;
    uint32_t name_length;
    bool has_parent;
    uint32_t parent_offset;
    uint32_t parent_length;
    ErrorProblem problem;
} ErrorDecl;

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
    bool for_range_ok;  /* a `..` here belongs to the header of the `for` being compiled */
    bool comma_list;    /* a `,` may separate this statement from the next (a `for` header) */
    bool range_case;    /* a range is being read as the item of a `case`: no step, no value pushed */
    int bracket_depth;  /* ( [ { opened and not yet closed, as tokens are consumed */
    int no_in_depth;    /* `in` is not an operator at this bracket depth (-1: nowhere): the
                           iterable of a `for ... in` header, where `in` is part of the loop */
    LoopFrame *loop; /* innermost while being compiled, NULL outside a loop */
    TryContext *try_ctx; /* innermost open try (or its finally region), see TryContext */
    FinallyGuard guard;
    ErrorDecl *error_decls; /* the `error` declarations of the file, in order, see prescan_errors */
    size_t error_decl_count;
    size_t error_decl_capacity;
    /* Classes. */
    LzString *class_name;     /* the class whose body is being compiled, or NULL */
    bool member_pending;      /* the next compile_function is a member of that class */
    bool member_static;       /* ...and it has no `self` */
    bool in_default;          /* compiling a field default or a constant initialiser */
    size_t class_parent_count; /* parents named in the head of the class being compiled */
    bool is_rhs;              /* parsing the type after `is`: an unknown name is "not a type" */
    bool in_trait;            /* the body being compiled is a trait's */
    bool in_extend;           /* the body being compiled is an `extend` block's */
    uint64_t error_class_kinds[4]; /* bit k: the error kind k has a class (declared in this file) */
    uint16_t extend_kinds;    /* ...and the builtin kinds it extends (0: a class) */
    uint16_t error_self_kind; /* the body being compiled is of an error class: its kind + 1, else 0 */
    bool last_bodiless;       /* the member function just compiled had no body (a trait's requirement) */
    LzString *classes[64];    /* the classes declared so far: valid names in a type */
    uint64_t class_ids[64];   /* their identities (LzClass.id), parallel to classes */
    uint64_t current_class_id; /* identity of the class whose body is being compiled, or 0 */
    size_t class_count;
} Parser;

/*
 * A `while` being compiled, so that `break` and `continue` in its body know where to
 * jump and what to pop. A function compiled inside the loop keeps the frame (the
 * error message names it), but its body may not jump to it: `func` is the function
 * that owns the loop.
 */
typedef struct LoopFrame {
    LoopFrame *enclosing;
    FuncState *func;
    size_t continue_target; /* the first byte of the condition */
    int scope_depth;        /* locals at or below it belong to the enclosing blocks */
    int continue_depth;     /* ...and these to the loop itself: `continue` keeps them */
    bool forward_continues; /* `continue` jumps forward to a step written after the body */
    size_t *breaks;        /* operand indexes of the JUMPs of every `break`, patched at the end */
    size_t break_count;
    size_t break_capacity;
    size_t *continues;     /* forward jumps of every `continue`, patched at the step */
    size_t continue_count;
    size_t continue_capacity;
} LoopFrame;

typedef enum Precedence {
    PREC_NONE,
    PREC_COALESCE, /* ?? */
    PREC_OR,
    PREC_AND,
    PREC_EQUALITY,
    PREC_COMPARISON,
    PREC_BIT_OR,  /* | */
    PREC_BIT_XOR, /* ^ */
    PREC_BIT_AND, /* & */
    PREC_SHIFT,   /* << >> */
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
    bool is_range;  /* the expression is `a..b`, left on the stack as start, end and step */
    bool inclusive; /* ...and `..=` counts the end */
    bool is_call;   /* the expression ends in a call: nothing is left on the stack */
    bool dict_value; /* `any` in type, but known to hold a dict */
    bool array_value; /* `any` in type, but known to hold an array */
    bool base_member; /* ...an index whose container came from a member: base_pos is that GET_FIELD */
    size_t base_pos;
    bool is_member; /* the expression is `a.name`: its last instruction is a GET_FIELD at op_pos */
    bool from_equality; /* the result of `==` or `!=` (`is` does not chain with them) */
    LzToken start;  /* the first token of the expression, for an error about the whole of it */
} Expr;

typedef Expr (*PrefixFn)(Parser *parser);
typedef Expr (*InfixFn)(Parser *parser, Expr left);

typedef struct ParseRule {
    PrefixFn prefix;
    InfixFn infix;
    Precedence precedence;
} ParseRule;

static void for_statement(Parser *parser);
static void block(Parser *parser);
static void end_statement(Parser *parser);
static void assignment_statement(Parser *parser);
static void store_variable(Parser *parser, LzString *name, const Symbol *symbol,
                           const LzToken *at);
static void step_statement(Parser *parser);
static bool member_assignment(Parser *parser, const Expr *target, bool finish);
static void declare_variable(Parser *parser, const LzToken *name_token, LzString *name,
                             bool is_const, const LzType *type);
static void patch_continues(Parser *parser, LoopFrame *loop);
static LzTokenType compound_operator(LzTokenType assign);
static LzOpCode compound_opcode(LzTokenType type);
static Expr compound_value(Parser *parser, const LzToken *at, const Expr *target,
                           const Expr *value);
static void variable_declaration(Parser *parser, bool is_const);
static void emit_try_exits(Parser *parser, bool loop_only, uint32_t offset);
static void patch_jump(Parser *parser, size_t operand_index);
static size_t emit_jump(Parser *parser, LzOpCode op, uint32_t offset);

static Expr expression(Parser *parser);
static Expr range_(Parser *parser, Expr left);
static void error_range_here(Parser *parser, const LzToken *at);
static void error_range_bound(Parser *parser, const LzToken *at, const LzType *type);
static bool is_compound(LzTokenType type);
static void declaration(Parser *parser);
static LzTokenType next_token_type(const Parser *parser);
static void end_statement(Parser *parser);
static Expr function_expression(Parser *parser);
static Expr parse_precedence(Parser *parser, Precedence precedence);

/* Diagnostics */

static void record_full(Parser *parser, uint32_t offset, uint32_t length, const char *message,
                        const char *label, const LzSuggestions *suggestions) {
    LzDiagnostics *diagnostics = parser->diagnostics;
    for (size_t i = 0; i < diagnostics->count; i++) {
        /* A finally body is compiled once per way out: the same mistake is told once. */
        if (diagnostics->items[i].offset == offset &&
            strcmp(diagnostics->items[i].message, message) == 0) {
            return;
        }
    }
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
    switch (parser->current.type) {
    case LZ_TOKEN_LEFT_PAREN:
    case LZ_TOKEN_LEFT_BRACKET:
    case LZ_TOKEN_LEFT_BRACE:
        parser->bracket_depth++;
        break;
    case LZ_TOKEN_RIGHT_PAREN:
    case LZ_TOKEN_RIGHT_BRACKET:
    case LZ_TOKEN_RIGHT_BRACE:
        parser->bracket_depth--;
        break;
    default:
        break;
    }
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
    symbol->loop_var = false;
    symbol->catch_var = false;
    symbol->sig = NULL;
    return symbol;
}

static void begin_scope(Parser *parser) {
    parser->scope_depth++;
}

/*
 * Drops every local declared inside the body of loop (the loop body's own scope and
 * anything nested in it), exactly as end_scope would when the block ended normally:
 * a captured local is closed, the rest are popped. The symbols stay in the table
 * because the code that follows is still compiled against them.
 */
static void discard_loop_locals(Parser *parser, LoopFrame *loop, uint32_t offset,
                                bool for_continue) {
    /* `continue` keeps what the loop keeps between rounds: its own variables and slots. */
    int keep = for_continue ? loop->continue_depth : loop->scope_depth;
    for (size_t i = parser->symbol_count; i > 0; i--) {
        Symbol *symbol = &parser->symbols[i - 1];
        if (symbol->depth <= keep) {
            break;
        }
        if (symbol->kind == SYMBOL_LOCAL) {
            emit_op_at(parser, symbol->captured ? LZ_OP_CLOSE_UPVALUE : LZ_OP_POP, offset);
        }
    }
}

/* Remembers where a `break`'s jump is, to be patched once the loop is complete. */
static void add_loop_break(Parser *parser, LoopFrame *loop, size_t operand) {
    if (loop->break_count == loop->break_capacity) {
        size_t capacity = loop->break_capacity != 0 ? loop->break_capacity * 2 : 4;
        size_t *grown = realloc(loop->breaks, capacity * sizeof *grown);
        if (grown == NULL) {
            fail(parser, LZ_ERR_MEMORY);
            return;
        }
        loop->breaks = grown;
        loop->break_capacity = capacity;
    }
    loop->breaks[loop->break_count++] = operand;
}

/* Remembers where a `continue`'s jump is, to be patched once the loop's step is known. */
static void add_loop_continue(Parser *parser, LoopFrame *loop, size_t operand) {
    if (loop->continue_count == loop->continue_capacity) {
        size_t capacity = loop->continue_capacity != 0 ? loop->continue_capacity * 2 : 4;
        size_t *grown = realloc(loop->continues, capacity * sizeof *grown);
        if (grown == NULL) {
            fail(parser, LZ_ERR_MEMORY);
            return;
        }
        loop->continues = grown;
        loop->continue_capacity = capacity;
    }
    loop->continues[loop->continue_count++] = operand;
}

/* Emits nothing: a `continue` of a `for` jumps forward to the step, patched later. */
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
    expr.is_range = false;
    expr.inclusive = false;
    expr.dict_value = false;
    expr.array_value = false;
    expr.is_call = false;
    expr.is_member = false;
    expr.from_equality = false;
    expr.base_member = false;
    expr.base_pos = 0;
    memset(&expr.start, 0, sizeof expr.start);
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
/* Would assigning `from` to `to` emit a runtime check (as opposed to a compile-time verdict)? */
static bool check_is_runtime(const LzType *from, const LzType *to) {
    if (lz_type_assignable(from, to) != LZ_ASSIGN_ALWAYS) {
        return true; /* MAYBE needs the check; NEVER is a compile error and needs nothing */
    }
    /*
     * An array stored under `T[]` is checked and restricted to T even when it
     * fits statically, so later pushes and stores into it are checked too.
     */
    bool array = (from->kinds & LZ_KIND_ARRAY) != 0 && (to->kinds & LZ_KIND_ARRAY) != 0 &&
                 to->element != NULL;
    bool dict = (from->kinds & LZ_KIND_DICT) != 0 && (to->kinds & LZ_KIND_DICT) != 0 &&
                (to->element != NULL || to->value_element != NULL);
    return array || dict;
}

/* `to` is turned into a constant and CHECK_TYPE is emitted: the top of the stack must fit it. */
static void emit_type_check(Parser *parser, const LzType *to, uint32_t offset) {
    uint16_t index = 0;
    if (!lz_chunk_add_type(parser->chunk, to, &index)) {
        if (parser->chunk->type_count >= LZ_TYPES_MAX) {
            error_at_previous(parser, "too many types in one chunk");
        } else {
            fail(parser, LZ_ERR_MEMORY);
        }
        return;
    }
    emit_op_at(parser, LZ_OP_CHECK_TYPE, offset);
    emit_u16_at(parser, index, offset);
}

/* Emits CHECK_TYPE for `to` unless the compile time already settled it. The top of the
 * stack must be the value being tested. */
static void emit_check_if_needed(Parser *parser, const LzType *from, const LzType *to,
                                 uint32_t offset) {
    if (lz_type_assignable(from, to) != LZ_ASSIGN_NEVER && check_is_runtime(from, to)) {
        emit_type_check(parser, to, offset);
    }
}

static void check_flow(Parser *parser, const LzType *from, const LzType *to, const LzToken *start,
                       uint32_t offset, LzOpCode check_op, const char *mismatch) {
    if (lz_type_assignable(from, to) == LZ_ASSIGN_NEVER) {
        LzToken span = span_from(parser, start);
        error_types(parser, &span, mismatch, to, ", got ", from);
        return;
    }
    if (!check_is_runtime(from, to)) {
        return;
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

/* The compile-time half of a check, for a place that has no value on the stack to test. */
static void check_static(Parser *parser, const LzType *from, const LzType *to,
                         const LzToken *start) {
    if (lz_type_assignable(from, to) == LZ_ASSIGN_NEVER) {
        LzToken span = span_from(parser, start);
        error_types(parser, &span, "type mismatch: expected ", to, ", got ", from);
    }
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

static bool is_any_type(const LzType *type);

/* The type `dict[keys, values]`; a limit or memory error is reported once. */
static LzType dict_type(Parser *parser, const LzType *keys, const LzType *values) {
    LzType shape = lz_type_of_kinds(LZ_KIND_DICT);
    LzType *stored = NULL;
    if (keys != NULL && !is_any_type(keys)) {
        if (lz_vm_intern_type(parser->vm, keys, &stored) != LZ_OK) {
            error_at_previous(parser, parser->vm->error);
            return shape;
        }
        shape.element = stored;
    }
    if (values != NULL && !is_any_type(values)) {
        if (lz_vm_intern_type(parser->vm, values, &stored) != LZ_OK) {
            error_at_previous(parser, parser->vm->error);
            return lz_type_of_kinds(LZ_KIND_DICT);
        }
        shape.value_element = stored;
    }
    if (shape.element == NULL && shape.value_element == NULL) {
        return shape;
    }
    LzType *found = NULL;
    LzStatus status = lz_vm_intern_type(parser->vm, &shape, &found);
    if (status == LZ_ERR_MEMORY || status == LZ_ERR_HEAP_LIMIT) {
        fail(parser, status);
        return lz_type_of_kinds(LZ_KIND_DICT);
    }
    if (status != LZ_OK) {
        error_at_previous(parser, parser->vm->error);
        return lz_type_of_kinds(LZ_KIND_DICT);
    }
    LzType out = lz_type_of_kinds(LZ_KIND_DICT);
    out.element = found->element;
    out.value_element = found->value_element;
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
    } else if (strcmp(name, "dict") == 0) {
        *out = lz_type_of_kinds(LZ_KIND_DICT);
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
                                                 "number", "any",   "never", "function",
                                                 "dict"};

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
            for (size_t i = 0; i < parser->class_count; i++) {
                if (parser->classes[i] == id) {
                    *out = lz_type_of_kinds(LZ_KIND_INSTANCE);
                    out->class_name = id; /* a subclass will match too (stage 2) */
                    out->class_id = parser->class_ids[i];
                    return true;
                }
            }
            /* A class made by an earlier source in this VM: the binding of the name right now. */
            const LzValue *bound = lz_table_get(&parser->vm->globals, id);
            if (bound != NULL && lz_value_is_class(bound)) {
                *out = lz_type_of_kinds(LZ_KIND_INSTANCE);
                out->class_name = id;
                out->class_id = lz_value_as_class(bound)->id;
                return true;
            }
            uint8_t error_kind;
            if (lz_vm_error_kind_from_name(parser->vm, parser->source->text + token.offset, token.length,
                                           &error_kind)) {
                *out = lz_type_of_kinds(LZ_KIND_ERROR); /* Error, ValueError, an error of the script... */
                out->class_id = (uint64_t)error_kind + 1;
                out->class_name = lz_vm_intern(parser->vm, lz_vm_error_kind_name(parser->vm, error_kind),
                                               strlen(lz_vm_error_kind_name(parser->vm, error_kind)));
                return true;
            }
            if (bound != NULL) {
                error_with_name(parser, &token, "'", "' is not a type");
                return false;
            }
            if (parser->is_rhs) {
                error_with_name(parser, &token, "'", "' is not a type");
                return false;
            }
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
        if (parser->previous.value.string.is_template) {
            error_at_previous(parser, "a template string cannot be used as a type");
            return false;
        }
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
        /* `int??` is `int?` written twice: nullable twice is nullable (the lexer made it `??`). */
        if (match(parser, LZ_TOKEN_QUESTION) || match(parser, LZ_TOKEN_QUESTION_QUESTION)) {
            LzType nil = lz_type_of_kinds(LZ_KIND_NIL);
            LzType merged;
            if (lz_type_union(&merged, out, &nil)) {
                *out = merged;
            }
        } else if (check(parser, LZ_TOKEN_LEFT_BRACKET) && out->kinds == LZ_KIND_DICT &&
                   out->literal_count == 0) {
            /* dict[key type, value type] */
            advance(parser);
            LzType keys = lz_type_of_kinds(LZ_KINDS_ANY);
            LzType values = lz_type_of_kinds(LZ_KINDS_ANY);
            LzToken keys_at = parser->current;
            bool pair = parse_type_union(parser, &keys);
            if (!match(parser, LZ_TOKEN_COMMA)) {
                error_at_current(parser, "dict[...] expects a key type and a value type");
                while (!check(parser, LZ_TOKEN_RIGHT_BRACKET) && !check(parser, LZ_TOKEN_EOF)) {
                    advance(parser);
                }
                consume(parser, LZ_TOKEN_RIGHT_BRACKET, "expected ']' after the dict types");
                *out = lz_type_of_kinds(LZ_KIND_DICT);
                return false;
            }
            skip_newlines(parser);
            pair = parse_type_union(parser, &values) && pair;
            consume(parser, LZ_TOKEN_RIGHT_BRACKET, "expected ']' after the dict types");
            uint16_t allowed = LZ_KIND_STRING | LZ_KIND_INT | LZ_KIND_BOOL;
            if ((lz_type_possible_kinds(&keys) & allowed) == 0) {
                error_types(parser, &keys_at, "", &keys, " cannot be a dict key", NULL);
            }
            *out = dict_type(parser, &keys, &values);
            ok = pair && ok;
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

/* A string constant on the stack, from decoded text. */
static Expr emit_string_value(Parser *parser, const char *data, size_t length) {
    LzString *text = lz_string_new(&parser->vm->heap, data, length);
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

static void error_range(Parser *parser, size_t offset, size_t length, const char *message) {
    if (parser->panic) {
        return;
    }
    parser->panic = true;
    record(parser, (uint32_t)offset, (uint32_t)(length == 0 ? 1 : length), message);
}

/*
 * Compiles source[start, end) as one expression, as if it were written there. The lexer
 * works on a copy of the source cut at end, so every offset stays the real one.
 */
static Expr embedded_expression(Parser *parser, size_t start, size_t end) {
    LzLexer saved_lexer = parser->lexer;
    LzToken saved_current = parser->current;
    LzToken saved_previous = parser->previous;
    const LzSource *saved_source = parser->source;
    int saved_depth = parser->bracket_depth;
    int saved_no_in = parser->no_in_depth;
    parser->no_in_depth = -1; /* a template's expression is not a `for` header, whatever holds it */

    LzSource cut = *saved_source;
    cut.length = end;
    parser->source = &cut;
    lz_lexer_init(&parser->lexer, &cut);
    parser->lexer.position = start;
    parser->lexer.nesting = 1; /* line breaks inside {} are not statement ends */
    parser->lexer.after_newline = false;

    advance(parser);
    Expr value = expression(parser);
    if (!check(parser, LZ_TOKEN_EOF)) {
        error_at_current(parser, "unexpected token in a template expression (expected '}')");
    }

    parser->lexer = saved_lexer;
    parser->current = saved_current;
    parser->previous = saved_previous;
    parser->source = saved_source;
    parser->bracket_depth = saved_depth;
    parser->no_in_depth = saved_no_in;
    return value;
}

/* The method of that name, called on the value below it: leaves [callee][value]. */
static bool emit_method_callee(Parser *parser, const char *name, size_t offset) {
    LzString *interned = lz_vm_intern(parser->vm, name, strlen(name));
    if (interned == NULL) {
        fail(parser, lz_vm_out_of_memory(parser->vm));
        return false;
    }
    const LzValue *method = lz_table_get(&parser->vm->methods, interned);
    if (method == NULL) {
        error_range(parser, offset, 1, "template strings need the builtin methods (to_string, format, to_debug)");
        return false;
    }
    (void)emit_constant(parser, *method);
    emit_op_at(parser, LZ_OP_SWAP, (uint32_t)offset);
    return true;
}

static void emit_call_count(Parser *parser, unsigned count, size_t offset) {
    emit_op_at(parser, LZ_OP_CALL, (uint32_t)offset);
    emit_byte_at(parser, (uint8_t)count, (uint32_t)offset);
}

/* An expression whose text form is on the stack. */
static void emit_as_text(Parser *parser, const Expr *value, size_t offset) {
    if (lz_type_possible_kinds(&value->type) != LZ_KIND_STRING &&
        emit_method_callee(parser, "to_string", offset)) {
        emit_call_count(parser, 1, offset);
    }
}

static void format_error(Parser *parser, size_t base, const LzFormatError *error) {
    if (parser->panic) {
        return;
    }
    LzSuggestions suggestions = {0};
    if (error->suggestion != 0) {
        suggestions.count = 1;
        suggestions.names[0][0] = error->suggestion;
        suggestions.names[0][1] = '\0';
    }
    parser->panic = true;
    record_full(parser, (uint32_t)(base + error->offset), (uint32_t)error->length, error->message,
                error->suggestion != 0 ? "not a format type" : NULL,
                error->suggestion != 0 ? &suggestions : NULL);
}

/* The spec of {x:spec} as one string on the stack; {w} parts are filled in at run time. */
static void compile_spec(Parser *parser, const LzStringPart *part) {
    const char *text = parser->source->text;
    size_t p = part->spec_start;
    size_t end = part->spec_end;

    if (memchr(text + p, '{', end - p) == NULL) {
        LzFormatSpec spec;
        LzFormatError error;
        if (!lz_format_parse(text + p, end - p, &spec, &error)) {
            format_error(parser, p, &error);
        }
        (void)emit_string_value(parser, text + p, end - p);
        return;
    }

    unsigned pieces = 0;
    while (p < end || pieces == 0) {
        size_t stop = p;
        while (stop < end && text[stop] != '{') {
            stop++;
        }
        if (stop > p || (pieces == 0 && stop >= end)) {
            (void)emit_string_value(parser, text + p, stop - p);
            if (pieces++ > 0) {
                emit_op_at(parser, LZ_OP_ADD, (uint32_t)p);
            }
        }
        if (stop >= end) {
            break;
        }
        LzStringBraces braces;
        LzStringError error;
        if (!lz_string_scan_braces(parser->source, stop, part->multiline, &braces, &error)) {
            error_range(parser, error.offset, error.length, error.message);
            return;
        }
        size_t first = stop + 1;
        while (first < braces.expr_end && (text[first] == ' ' || text[first] == '\t')) {
            first++;
        }
        if (first >= braces.expr_end) {
            error_range(parser, stop, braces.close - stop + 1, "empty '{}' in a format spec");
            return;
        }
        Expr value = embedded_expression(parser, stop + 1, braces.expr_end);
        emit_as_text(parser, &value, stop);
        if (pieces++ > 0) {
            emit_op_at(parser, LZ_OP_ADD, (uint32_t)stop);
        }
        p = braces.close + 1;
    }
}

/* One {...} of a template: leaves its text on the stack. */
static void interpolation(Parser *parser, const LzStringPart *part) {
    const char *text = parser->source->text;
    if (part->debug) {
        (void)emit_string_value(parser, text + part->open + 1, part->label_end - part->open - 1);
    }
    Expr value = embedded_expression(parser, part->expr_start, part->expr_end);

    if (part->has_spec) {
        if (emit_method_callee(parser, "format", part->open)) {
            compile_spec(parser, part);
            emit_call_count(parser, 2, part->open);
        }
    } else if (part->debug) {
        if (emit_method_callee(parser, "to_debug", part->open)) {
            emit_call_count(parser, 1, part->open);
        }
    } else {
        emit_as_text(parser, &value, part->open);
    }
    if (part->debug) {
        emit_op_at(parser, LZ_OP_ADD, (uint32_t)part->open);
    }
}

/* $"text {x} text": the pieces are joined left to right. */
static Expr template_string(Parser *parser) {
    LzStringReader reader;
    if (!lz_string_reader_init(&reader, parser->source, parser->previous.offset)) {
        error_at_previous(parser, "invalid template string");
        return any_expr();
    }

    LzBuffer text = {0};
    unsigned pieces = 0;
    Expr result = expr_of(lz_type_of_kinds(LZ_KIND_STRING));
    bool pure = true;
    for (;;) {
        LzStringPart part;
        text.length = 0;
        if (!lz_string_reader_next(&reader, &text, &part)) {
            fail(parser, lz_vm_out_of_memory(parser->vm));
            break;
        }
        if (part.kind == LZ_STRING_END) {
            break;
        }
        if (part.kind == LZ_STRING_ERROR) {
            error_range(parser, part.offset, part.length, part.message);
            break;
        }
        if (part.kind == LZ_STRING_TEXT) {
            result = emit_string_value(parser, text.data != NULL ? text.data : "", text.length);
        } else {
            interpolation(parser, &part);
            pure = false;
        }
        if (pieces++ > 0) {
            emit_op(parser, LZ_OP_ADD);
        }
    }
    if (pieces == 0) {
        result = emit_string_value(parser, "", 0);
    }
    lz_buffer_free(&text);
    return pure ? result : expr_of(lz_type_of_kinds(LZ_KIND_STRING));
}

static Expr string(Parser *parser) {
    if (parser->previous.value.string.is_template) {
        return template_string(parser);
    }
    LzBuffer decoded = {0};
    if (!lz_token_string_value(parser->source, &parser->previous, &decoded)) {
        lz_buffer_free(&decoded);
        fail(parser, LZ_ERR_MEMORY);
        return any_expr();
    }
    Expr result = emit_string_value(parser, decoded.data != NULL ? decoded.data : "", decoded.length);
    lz_buffer_free(&decoded);
    return result;
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

/* The expression that reads the variable named by `token`. */
static Expr call_args(Parser *parser, Expr callee, bool receiver);

/*
 * `super.name(args)` and `super.name`: the parent's method or property, with `self` as the
 * receiver. `super` stands for the parent of the class the code is written in, found when it runs.
 */
static Expr super_expr(Parser *parser, const LzToken *token) {
    if (parser->in_trait) {
        error_at(parser, token, "'super' cannot be used in a trait");
        return any_expr();
    }
    if (parser->in_extend) {
        error_at(parser, token, "'super' cannot be used in an extension");
        return any_expr();
    }
    bool method = false;
    bool static_function = false;
    for (const FuncState *f = parser->fs; f != NULL && !method; f = f->enclosing) {
        if (f->method_member) {
            method = true;
        } else if (f->static_member) {
            static_function = true;
            break;
        }
    }
    if (parser->class_name == NULL || parser->in_default || (!method && !static_function)) {
        error_at(parser, token, "'super' can only be used inside a method");
        return any_expr();
    }
    if (static_function) {
        error_at(parser, token, "'super' cannot be used in a static function");
        return any_expr();
    }
    if (parser->class_parent_count == 0) {
        char message[160];
        (void)snprintf(message, sizeof message, "%s has no parent class, so 'super' has nothing to refer to",
                       parser->class_name->data);
        error_at(parser, token, message);
        return any_expr();
    }
    if (!check(parser, LZ_TOKEN_DOT)) {
        error_at(parser, token, "'super' must be followed by .name");
        return any_expr();
    }
    advance(parser);
    if (!check(parser, LZ_TOKEN_IDENTIFIER)) {
        error_at_current(parser, "expected a method or property name after 'super.'");
        return any_expr();
    }
    advance(parser);
    LzToken member = parser->previous;
    LzString *member_name = intern_token(parser, &member);
    if (member_name == NULL) {
        return any_expr();
    }
    bool calls = check(parser, LZ_TOKEN_LEFT_PAREN);
    if (member_name->length == 4 && memcmp(member_name->data, "init", 4) == 0) {
        error_at(parser, &member, "init() runs automatically; super.init() is not needed");
        return any_expr();
    }
    if (!calls && (check(parser, LZ_TOKEN_EQUAL) || is_compound(parser->current.type))) {
        error_at_current(parser, "cannot assign through 'super'");
        return any_expr();
    }
    /* `self`: a parameter of the method, or captured by a lambda inside it. */
    LzString *self_name = lz_vm_intern(parser->vm, "self", 4);
    Symbol *self_symbol = self_name != NULL ? find_symbol(parser, self_name) : NULL;
    if (self_symbol == NULL || self_symbol->kind != SYMBOL_LOCAL) {
        error_at(parser, token, "'super' can only be used inside a method");
        return any_expr();
    }
    if (self_symbol->owner == parser->fs) {
        emit_op_at(parser, LZ_OP_GET_LOCAL, token->offset);
        emit_u16_at(parser, self_symbol->slot, token->offset);
    } else {
        int index = resolve_upvalue(parser, parser->fs, self_symbol);
        emit_op_at(parser, LZ_OP_GET_UPVALUE, token->offset);
        emit_u16_at(parser, (uint16_t)(index < 0 ? 0 : index), token->offset);
    }
    uint16_t name_index = make_constant(parser, lz_value_object(&member_name->base));
    if (!calls) {
        emit_op_at(parser, LZ_OP_SUPER, member.offset);
        emit_u16_at(parser, name_index, member.offset);
        emit_byte_at(parser, 1, member.offset);
        return any_expr();
    }
    emit_op_at(parser, LZ_OP_NIL, member.offset);
    emit_op_at(parser, LZ_OP_SUPER, member.offset);
    emit_u16_at(parser, name_index, member.offset);
    emit_byte_at(parser, 0, member.offset);
    emit_op_at(parser, LZ_OP_SWAP, member.offset);
    advance(parser); /* the '(' */
    Expr result = call_args(parser, any_expr(), true);
    Expr any = any_expr();
    any.is_call = result.is_call;
    return any;
}

static Expr variable_at(Parser *parser, const LzToken *token) {
    LzString *name = intern_token(parser, token);
    if (name == NULL) {
        return any_expr();
    }

    Symbol *symbol = find_symbol(parser, name);
    if (symbol == NULL && name->length == 5 && memcmp(name->data, "super", 5) == 0) {
        return super_expr(parser, token);
    }
    if (symbol != NULL) {
        switch (symbol->kind) {
        case SYMBOL_LOCAL:
            if (symbol->owner == parser->fs) {
                emit_op_at(parser, LZ_OP_GET_LOCAL, token->offset);
                emit_u16_at(parser, symbol->slot, token->offset);
            } else if (parser->fs->self_symbol != SIZE_MAX &&
                       symbol == &parser->symbols[parser->fs->self_symbol]) {
                /* A local function calling itself: no capture, so no reference cycle. */
                emit_op_at(parser, LZ_OP_GET_SELF, token->offset);
            } else {
                int index = resolve_upvalue(parser, parser->fs, symbol);
                emit_op_at(parser, LZ_OP_GET_UPVALUE, token->offset);
                emit_u16_at(parser, (uint16_t)(index < 0 ? 0 : index), token->offset);
            }
            break;
        case SYMBOL_GLOBAL:
            emit_named(parser, LZ_OP_GET_GLOBAL, name, token->offset);
            break;
        case SYMBOL_CONSTANT:
            emit_constant(parser, symbol->value);
            break;
        }
        Expr found = expr_of(symbol->type);
        found.sig = symbol->sig;
        found.dict_value = symbol->dict_value;
        found.array_value = symbol->array_value;
        return found;
    }

    if (name->length == 4 && memcmp(name->data, "self", 4) == 0) {
        if (parser->in_default) {
            error_at(parser, token, "a field default cannot use 'self'; set it in init() instead");
            return any_expr();
        }
        for (const FuncState *f = parser->fs; f != NULL; f = f->enclosing) {
            if (f->static_member) {
                error_at(parser, token,
                         "undefined name 'self' (static functions have no self; add 'self' as the first parameter)");
                return any_expr();
            }
        }
    }

    const LzValue *defined = lz_vm_lookup(parser->vm, name);
    if (defined == NULL) {
        /*
         * Sequential execution: a name that is not defined yet is not a compile error. The
         * program runs statement by statement and fails, with the source position, at the
         * statement that really uses a name that does not exist when it runs.
         */
        record_name_hint(parser, token, check(parser, LZ_TOKEN_LEFT_PAREN), false,
                        parser->method_name);
        emit_named(parser, LZ_OP_GET_GLOBAL, name, token->offset);
        return any_expr();
    }

    emit_named(parser, LZ_OP_GET_GLOBAL, name, token->offset);
    Expr expr = any_expr();
    if (defined->type == LZ_VAL_OBJECT && defined->as.object->type == LZ_OBJ_NATIVE) {
        expr.type = lz_type_of_kinds(LZ_KIND_FUNCTION);
        expr.native = (const LzNative *)defined->as.object;
    }
    return expr;
}

/* A variable read from the token just consumed. */
static Expr variable(Parser *parser) {
    return variable_at(parser, &parser->previous);
}

/* How a bitwise error names an operand: the same kind names the run-time error uses. */
static void describe_bit_operand(const LzType *type, char *out, size_t size) {
    LzType widened = lz_type_widen(type);
    uint16_t kinds = lz_type_possible_kinds(&widened);
    if (kinds != 0 && (kinds & (kinds - 1)) == 0) {
        (void)snprintf(out, size, "%s", lz_kind_name(kinds));
    } else {
        describe_type(&widened, out, size);
    }
}

/* "operator '&' needs two ints, got float and int", the message lz_arith_binary gives at run time. */
static void error_bitwise(Parser *parser, const LzToken *op, const LzType *left,
                          const LzType *right) {
    char symbol[16];
    char first[64];
    char second[64];
    char message[256];
    token_text(parser, op, symbol, sizeof symbol);
    describe_bit_operand(left, first, sizeof first);
    if (right == NULL) {
        (void)snprintf(message, sizeof message, "operator '%s' needs an int, got %s", symbol, first);
    } else {
        describe_bit_operand(right, second, sizeof second);
        const char *hint = "";
        if ((lz_type_widen(left).kinds == LZ_KIND_BOOL || lz_type_widen(right).kinds == LZ_KIND_BOOL)) {
            hint = op->type == LZ_TOKEN_AMPERSAND ? " (use 'and' for logic)"
                   : op->type == LZ_TOKEN_PIPE    ? " (use 'or' for logic)"
                                                  : "";
        }
        (void)snprintf(message, sizeof message, "operator '%s' needs two ints, got %s and %s%s",
                       symbol, first, second, hint);
    }
    error_at(parser, op, message);
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
    if (op.type == LZ_TOKEN_TILDE) {
        emit_op_at(parser, LZ_OP_BIT_NOT, op.offset);
        if ((lz_type_possible_kinds(&operand.type) & LZ_KIND_INT) == 0) {
            error_bitwise(parser, &op, &operand.type, NULL);
        }
        return expr_of(lz_type_of_kinds(LZ_KIND_INT));
    }
    emit_op_at(parser, op.type == LZ_TOKEN_BANG ? LZ_OP_NOT : LZ_OP_NEGATE, op.offset);
    if (op.type == LZ_TOKEN_BANG) {
        return expr_of(lz_type_of_kinds(LZ_KIND_BOOL));
    }

    uint16_t numbers = (uint16_t)(lz_type_possible_kinds(&operand.type) & LZ_KINDS_NUMBER);
    if (numbers == 0) {
        /* an object: its class may define the operator, which is looked for when this runs */
        if ((lz_type_possible_kinds(&operand.type) & LZ_KIND_INSTANCE) == 0) {
            error_operator(parser, &op, &operand.type, NULL);
        }
        return any_expr();
    }
    return expr_of(lz_type_of_kinds(numbers));
}

/*
 * Skips forward to the `}` that closes the branch, so one mistake in a branch is
 * one error and the rest of the expression still parses.
 */
static void if_skip_to_close(Parser *parser) {
    int depth = 0;
    while (!check(parser, LZ_TOKEN_EOF)) {
        if (check(parser, LZ_TOKEN_LEFT_BRACE)) {
            depth++;
        } else if (check(parser, LZ_TOKEN_RIGHT_BRACE)) {
            if (depth == 0) {
                advance(parser);
                return;
            }
            depth--;
        }
        advance(parser);
    }
}

/*
 * The `{ ... }` of one branch: exactly one expression, and its value stays on the
 * stack. A statement where the expression should be is an error; the branch is
 * then skipped whole so the rest of the expression still parses.
 */
static Expr if_expression_branch(Parser *parser) {
    static const LzTokenType statements[] = {LZ_TOKEN_LET,  LZ_TOKEN_CONST,  LZ_TOKEN_WHILE,
                                             LZ_TOKEN_FOR,  LZ_TOKEN_RETURN, LZ_TOKEN_BREAK,
                                             LZ_TOKEN_CONTINUE};
    advance(parser); /* the '{' */
    skip_newlines(parser);
    for (size_t i = 0; i < sizeof statements / sizeof statements[0]; i++) {
        if (check(parser, statements[i])) {
            error_at_current(parser,
                             "a branch of an 'if' expression holds one expression, not a statement");
            if_skip_to_close(parser);
            return any_expr();
        }
    }
    if (check(parser, LZ_TOKEN_RIGHT_BRACE) || check(parser, LZ_TOKEN_EOF)) {
        error_at_current(parser, "expected an expression in the branch of an 'if' expression");
        if (check(parser, LZ_TOKEN_RIGHT_BRACE)) {
            advance(parser); /* an empty branch still closes, so the `else` is read */
        }
        return any_expr();
    }
    Expr value = expression(parser);
    skip_newlines(parser);
    if (!check(parser, LZ_TOKEN_RIGHT_BRACE)) {
        error_at_current(parser, "expected '}' after the expression");
        if_skip_to_close(parser);
        return value;
    }
    advance(parser);
    return value;
}

/* The branch that follows `if <condition>` whose `{` the caller has just seen. */
static Expr if_then_branch(Parser *parser) {
    return if_expression_branch(parser);
}

/*
 * `if cond { expr } else { expr }` in an expression position, with `else if`
 * chains. It is a prefix expression, so an operator that follows the last `}`
 * applies to the whole of it. Each branch leaves exactly one value on the stack
 * and only the branch taken runs: the same jumps the statement uses, except that
 * nothing pops the branch's value, so the value of the branch taken is what stays.
 */
static Expr if_expression(Parser *parser) {
    size_t *end_jumps = NULL;
    size_t end_count = 0;
    size_t end_capacity = 0;
    LzToken if_token = parser->previous;
    Expr result = any_expr();
    bool first = true;

    for (;;) {
        uint32_t offset = parser->previous.offset;
        (void)expression(parser);
        if (!check(parser, LZ_TOKEN_LEFT_BRACE)) {
            error_at_current(parser, "expected '{' after the condition");
            /* Leave the caller at the end of the statement: the rest is not read again. */
            while (!check(parser, LZ_TOKEN_NEWLINE) && !check(parser, LZ_TOKEN_EOF)) {
                advance(parser);
            }
            break;
        }
        size_t then_jump = emit_jump(parser, LZ_OP_JUMP_IF_FALSE, offset);
        emit_op_at(parser, LZ_OP_POP, offset);

        Expr branch = if_then_branch(parser);
        result = first ? branch : union_expr(result, branch);
        first = false;

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
            /* The caret goes on the `if` that opened the expression. */
            error_at(parser, &if_token, "an 'if' expression needs an 'else'");
            break;
        }
        if (match(parser, LZ_TOKEN_IF)) {
            continue;
        }
        if (!check(parser, LZ_TOKEN_LEFT_BRACE)) {
            error_at_current(parser, "expected '{' or 'if' after 'else'");
            break;
        }
        result = union_expr(result, if_then_branch(parser));
        break;
    }

    for (size_t i = 0; i < end_count; i++) {
        patch_jump(parser, end_jumps[i]);
    }
    free(end_jumps);
    return result;
}

static bool key_kind_ok(const LzType *type);
static void error_key(Parser *parser, const LzToken *at, const LzType *type);

/*
 * Does the `{` after `=>` open a dict literal? `{"a": 1}` and `{}` do; a block of
 * statements does not. A statement keyword or a ';' settles it, otherwise the first
 * `:` outside any bracket does.
 */
static bool arrow_starts_dict(Parser *parser) {
    LzLexer ahead = parser->lexer;
    LzToken token = lz_lexer_next(&ahead);
    if (token.type == LZ_TOKEN_RIGHT_BRACE) {
        return true;
    }
    switch (token.type) {
    case LZ_TOKEN_LET:
    case LZ_TOKEN_CONST:
    case LZ_TOKEN_IF:
    case LZ_TOKEN_SWITCH:
    case LZ_TOKEN_TRY:
    case LZ_TOKEN_THROW:
    case LZ_TOKEN_WHILE:
    case LZ_TOKEN_FOR:
    case LZ_TOKEN_RETURN:
    case LZ_TOKEN_BREAK:
    case LZ_TOKEN_CONTINUE:
        return false;
    default:
        break;
    }
    int depth = 0;
    for (;;) {
        if (token.type == LZ_TOKEN_EOF) {
            return false;
        }
        if (depth == 0) {
            if (token.type == LZ_TOKEN_COLON) {
                return true;
            }
            if (token.type == LZ_TOKEN_RIGHT_BRACE || token.type == LZ_TOKEN_SEMICOLON) {
                return false;
            }
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

/*
 * The key a literal token stands for, when the whole key expression was that one
 * token: `"a"`, 1 and true are known at compile time, `k` or `"a" + "b"` are not.
 */
static bool key_literal(Parser *parser, const LzToken *token, LzLiteral *out) {
    if (token->offset + token->length != parser->previous.offset + parser->previous.length ||
        token->offset != parser->previous.offset) {
        return false;
    }
    switch (parser->previous.type) {
    case LZ_TOKEN_INTEGER:
        out->kind = LZ_VAL_INT;
        out->as.integer = parser->previous.value.integer;
        return true;
    case LZ_TOKEN_TRUE:
    case LZ_TOKEN_FALSE:
        out->kind = LZ_VAL_BOOL;
        out->as.boolean = parser->previous.type == LZ_TOKEN_TRUE;
        return true;
    case LZ_TOKEN_STRING: {
        if (parser->previous.value.string.is_template) {
            return false;
        }
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
        out->kind = LZ_VAL_OBJECT;
        out->as.string = text;
        return true;
    }
    default:
        return false;
    }
}

static bool same_key(const LzLiteral *a, const LzLiteral *b) {
    if (a->kind != b->kind) {
        return false;
    }
    switch (a->kind) {
    case LZ_VAL_INT:
        return a->as.integer == b->as.integer;
    case LZ_VAL_BOOL:
        return a->as.boolean == b->as.boolean;
    default:
        return a->as.string == b->as.string;
    }
}

/* The key as the message shows it: a string quoted, a number and a bool bare. */
static void describe_key(const LzLiteral *literal, char *out, size_t size) {
    LzValue value;
    if (literal->kind == LZ_VAL_INT) {
        value = lz_value_int(literal->as.integer);
    } else if (literal->kind == LZ_VAL_BOOL) {
        value = lz_value_bool(literal->as.boolean);
    } else {
        value = lz_value_object((LzObject *)literal->as.string);
    }
    LzBuffer text = {0};
    LzWriter writer = lz_writer_from_buffer(&text);
    lz_value_write_debug(&value, &writer);
    (void)snprintf(out, size, "%s", text.data != NULL ? text.data : "?");
    lz_buffer_free(&text);
}

/*
 * {...}: a dict literal, in expression position only. The keys and values go on the
 * stack in source order and OP_DICT builds the dict from them. A duplicate literal key
 * is a compile error; a duplicate computed key stores over the first (last wins).
 */
static Expr dict_literal(Parser *parser) {
    uint32_t offset = parser->previous.offset;
    unsigned count = 0;
    LzType keys = lz_type_of_kinds(0);
    LzType values = lz_type_of_kinds(0);
    LzLiteral seen[32];
    size_t seen_count = 0;

    skip_newlines(parser);
    if (!check(parser, LZ_TOKEN_RIGHT_BRACE)) {
        do {
            skip_newlines(parser);
            if (check(parser, LZ_TOKEN_RIGHT_BRACE)) {
                break; /* trailing comma */
            }
            LzToken key_at = parser->current;
            Expr key = expression(parser);
            if (!key_kind_ok(&key.type)) {
                error_key(parser, &key_at, &key.type);
            }
            LzLiteral literal;
            if (key_literal(parser, &key_at, &literal)) {
                bool duplicate = false;
                for (size_t i = 0; i < seen_count && !duplicate; i++) {
                    duplicate = same_key(&seen[i], &literal);
                }
                if (duplicate) {
                    char text[48];
                    char message[128];
                    describe_key(&literal, text, sizeof text);
                    (void)snprintf(message, sizeof message, "duplicate key %s in the dict literal",
                                   text);
                    error_at(parser, &key_at, message);
                } else if (seen_count < sizeof seen / sizeof seen[0]) {
                    seen[seen_count++] = literal;
                }
            }
            LzType widened = lz_type_widen(&key.type);
            LzType merged;
            if (lz_type_union(&merged, &keys, &widened)) {
                keys = merged;
            }

            if (!match(parser, LZ_TOKEN_COLON)) {
                error_at_current(parser, "expected ':' after the key in the dict literal");
                return expr_of(lz_type_of_kinds(LZ_KIND_DICT));
            }
            skip_newlines(parser);
            if (check(parser, LZ_TOKEN_RIGHT_BRACE) || check(parser, LZ_TOKEN_EOF)) {
                error_at_current(parser, "expected a value after ':'");
                return expr_of(lz_type_of_kinds(LZ_KIND_DICT));
            }
            Expr value = expression(parser);
            LzType value_widened = lz_type_widen(&value.type);
            if (lz_type_union(&merged, &values, &value_widened)) {
                values = merged;
            }
            if (++count > 0xFFFF) {
                error_at(parser, &key_at, "too many entries in one dict literal");
                count = 0xFFFF;
            }
            skip_newlines(parser);
        } while (match(parser, LZ_TOKEN_COMMA));
    }
    skip_newlines(parser);
    consume(parser, LZ_TOKEN_RIGHT_BRACE, "expected '}' after the dict literal");

    emit_op_at(parser, LZ_OP_DICT, offset);
    emit_u16_at(parser, (uint16_t)count, offset);
    return expr_of(dict_type(parser, &keys, &values));
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

static bool is_any_type(const LzType *type);

/* The static type of the values of a dict, or any. */
static LzType dict_value_type(const LzType *container) {
    if ((container->kinds & LZ_KIND_DICT) != 0 && container->value_element != NULL) {
        return *container->value_element;
    }
    return lz_type_of_kinds(LZ_KINDS_ANY);
}

/* An index that can never be a key: a float, nil, an array or a dict. */
static bool key_kind_ok(const LzType *type) {
    return (lz_type_possible_kinds(type) & (LZ_KIND_STRING | LZ_KIND_INT | LZ_KIND_BOOL)) != 0;
}

static void error_key(Parser *parser, const LzToken *at, const LzType *type) {
    char text[96];
    describe_bit_operand(type, text, sizeof text);
    char message[160];
    (void)snprintf(message, sizeof message, "%s %s cannot be a dict key",
                   strcmp(text, "error") == 0 ? "an" : "a", text);
    error_at(parser, at, message);
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

    uint16_t container_kinds = lz_type_possible_kinds(&left.type);
    if ((container_kinds & (LZ_KIND_ARRAY | LZ_KIND_DICT)) == 0) {
        error_types(parser, &bracket, "cannot index a value of type ", &left.type, "", NULL);
    }
    /* Only a value that is a dict for sure reads a key. An `any` stays array-like: the
     * index must be an int there, and the run time also accepts a dict key. */
    bool dict = container_kinds == LZ_KIND_DICT ||
                (left.dict_value && container_kinds == LZ_KINDS_ANY);
    /* A value of unknown kind may be either; only the run time can say which. */
    bool array = container_kinds == LZ_KIND_ARRAY ||
                 (left.array_value && container_kinds == LZ_KINDS_ANY);

    bool has_first = !check(parser, LZ_TOKEN_COLON);
    Expr first = expr_of(lz_type_of_kinds(LZ_KIND_NIL));
    if (has_first) {
        first = expression(parser);
    } else {
        emit_op_at(parser, LZ_OP_NIL, offset);
    }

    if (match(parser, LZ_TOKEN_COLON)) {
        if (dict) {
            error_at(parser, &bracket, "cannot use a dict here");
        }
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

    if (dict) {
        if (!key_kind_ok(&first.type)) {
            error_key(parser, &bracket, &first.type);
        } else if ((left.type.kinds & LZ_KIND_DICT) != 0 && left.type.element != NULL &&
                   lz_type_assignable(&first.type, left.type.element) == LZ_ASSIGN_NEVER) {
            error_types(parser, &bracket, "a dict key of type ", left.type.element,
                        " cannot hold ", &first.type);
        }
    } else if (array) {
        check_bound(parser, &bracket, &first, false);
    }
    consume(parser, LZ_TOKEN_RIGHT_BRACKET, "expected ']' after index");
    Expr result = expr_of(dict ? dict_value_type(&left.type) : item_type_of(&left.type));
    result.dict_value = dict;
    result.array_value = !dict && array && left.type.element != NULL;
    result.is_index = true;
    result.base_member = left.is_member;
    result.base_pos = left.op_pos;
    result.op_pos = parser->chunk->count;
    /*
     * `a[i] op= v` needs the array and the index under the item, so that OP_SET_INDEX
     * can store over it. The token after the expression tells which form to emit.
     */
    if (is_compound(parser->current.type)) {
        emit_op_at(parser, LZ_OP_INDEX_KEEP, offset);
        result.op_pos = parser->chunk->count - 1;
    } else {
        emit_op_at(parser, LZ_OP_INDEX, offset);
    }
    return result;
}

static Expr binary(Parser *parser, Expr left);
static Expr call(Parser *parser, Expr callee);
static Expr dot_call(Parser *parser, Expr receiver);
static Expr and_(Parser *parser, Expr left);
static Expr or_(Parser *parser, Expr left);
static Expr in_(Parser *parser, Expr left);
static Expr coalesce(Parser *parser, Expr left);
static Expr if_expression(Parser *parser);
static Expr switch_expression(Parser *parser);
static void switch_statement(Parser *parser);
static void try_statement(Parser *parser);
static void throw_statement(Parser *parser);

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
    case LZ_TOKEN_LEFT_BRACE:
        return (ParseRule){dict_literal, NULL, PREC_NONE};
    case LZ_TOKEN_DOT:
        return (ParseRule){NULL, dot_call, PREC_CALL};
    case LZ_TOKEN_FN:
        return (ParseRule){function_expression, NULL, PREC_NONE};
    case LZ_TOKEN_IF:
        return (ParseRule){if_expression, NULL, PREC_NONE};
    case LZ_TOKEN_SWITCH:
        return (ParseRule){switch_expression, NULL, PREC_NONE};
    case LZ_TOKEN_BANG:
    case LZ_TOKEN_TILDE:
        return (ParseRule){unary, NULL, PREC_NONE};
    case LZ_TOKEN_PIPE:
        return (ParseRule){NULL, binary, PREC_BIT_OR};
    case LZ_TOKEN_CARET:
        return (ParseRule){NULL, binary, PREC_BIT_XOR};
    case LZ_TOKEN_AMPERSAND:
        return (ParseRule){NULL, binary, PREC_BIT_AND};
    case LZ_TOKEN_LESS_LESS:
    case LZ_TOKEN_GREATER_GREATER:
        return (ParseRule){NULL, binary, PREC_SHIFT};
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
    case LZ_TOKEN_DOT_DOT:
    case LZ_TOKEN_DOT_DOT_EQUAL:
        return (ParseRule){NULL, range_, PREC_OR};
    case LZ_TOKEN_IN:
    case LZ_TOKEN_NOT_IN:
        return (ParseRule){NULL, in_, PREC_COMPARISON};
    case LZ_TOKEN_QUESTION_QUESTION:
        return (ParseRule){NULL, coalesce, PREC_COALESCE};
    case LZ_TOKEN_AND:
        return (ParseRule){NULL, and_, PREC_AND};
    case LZ_TOKEN_OR:
        return (ParseRule){NULL, or_, PREC_OR};
    default:
        return (ParseRule){NULL, NULL, PREC_NONE};
    }
}

static bool word_is(const Parser *parser, const LzToken *token, const char *word);

/*
 * `value is T` / `value is not T`. The right side is a type, read by the parser of annotations, and
 * the test is the one an annotation makes, so `x is T` holds exactly when `: T` would accept x.
 * Like `==`, it does not chain: `a is B == c` needs parentheses.
 */
static Expr is_(Parser *parser, Expr left, const LzToken *is_token) {
    Expr result = expr_of(lz_type_of_kinds(LZ_KIND_BOOL));
    if (left.from_equality) {
        error_at(parser, is_token, "'is' cannot follow '==' or '!='; add parentheses");
        return result;
    }
    bool negate = match(parser, LZ_TOKEN_BANG);
    skip_newlines(parser);
    LzType type;
    bool saved = parser->is_rhs;
    parser->is_rhs = true;
    bool ok = parse_type_union(parser, &type);
    parser->is_rhs = saved;
    if (!ok) {
        return result;
    }
    uint16_t index = 0;
    if (!lz_chunk_add_type(parser->chunk, &type, &index)) {
        if (parser->chunk->type_count >= LZ_TYPES_MAX) {
            error_at_previous(parser, "too many types in one chunk");
        } else {
            fail(parser, LZ_ERR_MEMORY);
        }
        return result;
    }
    emit_op_at(parser, LZ_OP_IS, is_token->offset);
    emit_u16_at(parser, index, is_token->offset);
    if (negate) {
        emit_op_at(parser, LZ_OP_NOT, is_token->offset);
    }
    if (check(parser, LZ_TOKEN_EQUAL_EQUAL) || check(parser, LZ_TOKEN_BANG_EQUAL) ||
        (check(parser, LZ_TOKEN_IDENTIFIER) && word_is(parser, &parser->current, "is"))) {
        error_at_current(parser, "'is' cannot be chained with '==', '!=' or another 'is'; add parentheses");
    }
    return result;
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
    LzToken first = parser->current;
    advance(parser);
    Expr left = prefix(parser);
    left.start = first;
    for (;;) {
        /* A chain may continue on the next line:  xs\n  .filter(...)\n  .map(...) */
        if (check(parser, LZ_TOKEN_NEWLINE) && next_token_type(parser) == LZ_TOKEN_DOT) {
            advance(parser);
        }
        /* `x is T` and `x is not T`: `is` is a word only here, after an expression on its line. */
        if (parser->fatal == LZ_OK && precedence <= PREC_EQUALITY && check(parser, LZ_TOKEN_IDENTIFIER) &&
            word_is(parser, &parser->current, "is")) {
            LzToken is_token = parser->current;
            advance(parser);
            left = is_(parser, left, &is_token);
            continue;
        }
        if (parser->fatal != LZ_OK || precedence > rule_for(parser->current.type).precedence) {
            break;
        }
        /* The `in` of a `for` header belongs to the loop, not to the iterable's expression. */
        if ((parser->current.type == LZ_TOKEN_IN || parser->current.type == LZ_TOKEN_NOT_IN) &&
            parser->no_in_depth == parser->bracket_depth) {
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
    return parse_precedence(parser, PREC_COALESCE);
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
        /* an object: its class may define the operator, which is looked for when this runs */
        if (((a | b) & LZ_KIND_INSTANCE) == 0) {
            error_operator(parser, op, &left->type, &right->type);
        }
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
    case LZ_TOKEN_AMPERSAND:
    case LZ_TOKEN_PIPE:
    case LZ_TOKEN_CARET:
    case LZ_TOKEN_LESS_LESS:
    case LZ_TOKEN_GREATER_GREATER: {
        LzOpCode code = op.type == LZ_TOKEN_AMPERSAND ? LZ_OP_BIT_AND
                        : op.type == LZ_TOKEN_PIPE    ? LZ_OP_BIT_OR
                        : op.type == LZ_TOKEN_CARET   ? LZ_OP_BIT_XOR
                        : op.type == LZ_TOKEN_LESS_LESS ? LZ_OP_SHL
                                                        : LZ_OP_SHR;
        emit_op_at(parser, code, op.offset);
        if ((lz_type_possible_kinds(&left.type) & LZ_KIND_INT) == 0 ||
            (lz_type_possible_kinds(&right.type) & LZ_KIND_INT) == 0) {
            error_bitwise(parser, &op, &left.type, &right.type);
        }
        return expr_of(lz_type_of_kinds(LZ_KIND_INT));
    }
    case LZ_TOKEN_EQUAL_EQUAL: {
        emit_op_at(parser, LZ_OP_EQUAL, op.offset);
        Expr equal = expr_of(lz_type_of_kinds(LZ_KIND_BOOL));
        equal.from_equality = true;
        return equal;
    }
    case LZ_TOKEN_BANG_EQUAL: {
        emit_op_at(parser, LZ_OP_EQUAL, op.offset);
        emit_op_at(parser, LZ_OP_NOT, op.offset);
        Expr unequal = expr_of(lz_type_of_kinds(LZ_KIND_BOOL));
        unequal.from_equality = true;
        return unequal;
    }
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
    if (((a & LZ_KINDS_NUMBER) == 0 || (b & LZ_KINDS_NUMBER) == 0) && ((a | b) & LZ_KIND_INSTANCE) == 0) {
        error_operator(parser, &op, &left.type, &right.type);
    }
    return expr_of(lz_type_of_kinds(LZ_KIND_BOOL));
}

static bool is_any_type(const LzType *type) {
    return type->kinds == LZ_KINDS_ANY && type->literal_count == 0 && type->element == NULL;
}

/* Is the argument list that starts at the current token one with `*spread` or `name = value`? */
static bool call_has_special_args(const Parser *parser, bool colon_names) {
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
                    LzTokenType after = lz_lexer_next(&peek).type;
                    if (after == LZ_TOKEN_EQUAL || (colon_names && after == LZ_TOKEN_COLON)) {
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
    uint8_t error_kind_made = 0;
    bool makes_error = callee.native != NULL && !receiver && lz_native_error_kind(callee.native, &error_kind_made);
    /* An error with a class takes its fields by name too: the call checks them when it runs. */
    bool error_has_class = makes_error &&
                           (((parser->error_class_kinds[error_kind_made / 64] >> (error_kind_made % 64)) & 1u) != 0 ||
                            lz_vm_error_class(parser->vm, error_kind_made) != NULL);
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
            } else if (check(parser, LZ_TOKEN_IDENTIFIER) &&
                       (next_token_type(parser) == LZ_TOKEN_EQUAL || next_token_type(parser) == LZ_TOKEN_COLON)) {
                advance(parser);
                LzToken name_token = parser->previous;
                LzString *name = intern_token(parser, &name_token);
                advance(parser); /* the '=' or ':' */
                skip_newlines(parser);
                for (size_t i = 0; name != NULL && i < named; i++) {
                    if (seen[i] == name) {
                        error_with_name(parser, &name_token, "argument '", "' is given twice");
                    }
                }
                if (makes_error && error_has_class) {
                    /* checked when the call runs */
                } else if (makes_error) {
                    bool known = name != NULL && ((name->length == 5 && memcmp(name->data, "cause", 5) == 0) ||
                                                  (name->length == 4 && memcmp(name->data, "data", 4) == 0));
                    if (!known) {
                        LzSuggester suggester;
                        lz_suggester_init(&suggester, parser->source->text + name_token.offset,
                                          name_token.length);
                        lz_suggester_offer(&suggester, "cause");
                        lz_suggester_offer(&suggester, "data");
                        error_with_name_suggest(parser, &name_token, "unexpected named argument '",
                                                "'", "an error takes only cause and data",
                                                &suggester.found);
                    }
                } else if (callee.native != NULL) {
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
    Expr result = call_result(callee);
    result.is_call = true;
    return result;
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
        if (lz_type_possible_kinds(&callee.type) == LZ_KIND_DICT) {
            error_at(parser, &paren, "a dict is not callable");
        } else {
            error_types(parser, &paren, "cannot call a value of type ", &callee.type, "", NULL);
        }
    }
    if (call_has_special_args(parser, true)) {
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
    Expr result = call_result(callee);
    result.is_call = true;
    return result;
}

static Expr call(Parser *parser, Expr callee) {
    return call_args(parser, callee, false);
}

/*
 * Whether `receiver.name` may be a member of an object (or a class). A receiver whose type
 * is known and cannot be an object keeps the older error/builtin handling; anything else,
 * including every value of unknown type (a parameter, a result from another file), takes the
 * conservative path and is typed `any`. A VM that has seen a class keeps that path for all
 * receivers, as before.
 */
static bool receiver_may_be_object(const Parser *parser, const Expr *receiver) {
    return parser->vm->has_classes || (lz_type_possible_kinds(&receiver->type) & LZ_KIND_INSTANCE) != 0;
}

/*
 * receiver.name(args) is name(receiver, args): the callee is looked up like any other
 * name (locals, functions of the script, builtins), pushed above the receiver, and the
 * two are swapped so the call finds [callee][receiver][args...].
 */
static Expr dot_call(Parser *parser, Expr receiver) {
    if (!check(parser, LZ_TOKEN_IDENTIFIER)) {
        error_at_current(parser, "expected a method name after '.'");
        return any_expr();
    }
    advance(parser);
    LzToken name = parser->previous;
    if (!check(parser, LZ_TOKEN_LEFT_PAREN)) {
        /* `value.name` with no parentheses is a field of an error, or of an object. */
        bool classes = receiver_may_be_object(parser, &receiver);
        if (!classes && (check(parser, LZ_TOKEN_EQUAL) || is_compound(parser->current.type))) {
            error_at_current(parser, "cannot assign to a field of an error");
            return any_expr();
        }
        LzString *field = intern_token(parser, &name);
        if (field == NULL) {
            return any_expr();
        }
        if (classes) {
            Expr member = any_expr();
            member.is_member = true;
            if (is_compound(parser->current.type)) {
                emit_named(parser, LZ_OP_GET_FIELD_KEEP, field, name.offset);
                member.op_pos = parser->chunk->count - 3;
            } else {
                member.op_pos = parser->chunk->count;
                emit_named(parser, LZ_OP_GET_FIELD, field, name.offset);
            }
            return member;
        }
        emit_named(parser, LZ_OP_GET_FIELD, field, name.offset);
        if (field->length == 7 && memcmp(field->data, "message", 7) == 0) {
            return expr_of(lz_type_of_kinds(LZ_KIND_STRING));
        }
        if (field->length == 4 && memcmp(field->data, "kind", 4) == 0) {
            return expr_of(lz_type_of_kinds(LZ_KIND_STRING));
        }
        if (field->length == 5 && memcmp(field->data, "trace", 5) == 0) {
            return expr_of(lz_type_of_kinds(LZ_KIND_ARRAY));
        }
        if (field->length == 5 && memcmp(field->data, "cause", 5) == 0) {
            return expr_of(lz_type_of_kinds(LZ_KIND_ERROR | LZ_KIND_NIL));
        }
        return any_expr(); /* data, or a field an error does not have */
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
    bool classes = receiver_may_be_object(parser, &receiver);
    if (!classes && (lz_type_possible_kinds(&receiver.type) & LZ_KIND_ERROR) != 0 && interned != NULL &&
        ((interned->length == 4 && memcmp(interned->data, "with", 4) == 0) ||
         (interned->length == 4 && memcmp(interned->data, "copy", 4) == 0))) {
        classes = true; /* every error has copy() and with(): the run-time lookup finds them */
    }
    int lookup_mode = 0;
    if (method != NULL) {
        (void)emit_constant(parser, *method);
        callee = expr_of(lz_type_of_kinds(LZ_KIND_FUNCTION));
        callee.native = (const LzNative *)method->as.object;
    } else if (classes && interned != NULL && find_symbol(parser, interned) == NULL &&
               lz_vm_lookup(parser->vm, interned) == NULL) {
        /* No function of that name is known: an object's own method may answer, else the lookup
         * fails at run time like an undefined name does. */
        record_name_hint(parser, &name, true, false, true);
        emit_op_at(parser, LZ_OP_NIL, name.offset);
        callee = any_expr();
        lookup_mode = 1;
    } else {
        parser->method_name = true;
        callee = variable(parser);
        parser->method_name = false;
    }
    if (classes && interned != NULL) {
        /* The receiver may be an object or a class: its own member beats the callee above. */
        emit_op_at(parser, LZ_OP_METHOD_LOOKUP, name.offset);
        emit_u16_at(parser, make_constant(parser, lz_value_object(&interned->base)), name.offset);
        emit_byte_at(parser, (uint8_t)lookup_mode, name.offset);
        callee.sig = NULL;
        callee.native = NULL;
    }
    emit_op_at(parser, LZ_OP_SWAP, name.offset);
    advance(parser); /* the '(' */
    /* `d.copy()` gives a dict: `x["a"]` on it reads a key. The receiver says whether the
     * dict row of the method answered, which `result_kinds` alone cannot tell. */
    bool dict_receiver = receiver.dict_value ||
                         lz_type_possible_kinds(&receiver.type) == LZ_KIND_DICT;
    Expr result = call_args(parser, callee, true);
    if (classes) {
        bool is_call = result.is_call;
        result = any_expr(); /* the receiver's own method may return anything */
        result.is_call = is_call;
        return result;
    }
    result.dict_value = dict_receiver && callee.native != NULL &&
                        (callee.native->result_kinds & LZ_KIND_DICT) != 0;
    return result;
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

/*
 * `x in xs`, `x not in xs` and `x in a..b`. The operator sits with the comparisons. A range
 * is only allowed right after `in`, where it becomes one instruction over [x][a][b], so each
 * of the three is evaluated once, left to right. Anything else on the right is a container:
 * the instruction itself says which, and what it cannot be.
 */
static Expr in_(Parser *parser, Expr left) {
    LzToken op = parser->previous;
    bool negate = op.type == LZ_TOKEN_NOT_IN;
    Expr result = expr_of(lz_type_of_kinds(LZ_KIND_BOOL));
    skip_newlines(parser);
    if (rule_for(parser->current.type).prefix == NULL) {
        error_at_current(parser, "expected an expression after 'in'");
        return result;
    }

    LzToken right_at = parser->current;
    Expr right = parse_precedence(parser, PREC_BIT_OR);
    if (right.start.length != 0) {
        right_at = right.start;
    }
    LzType left_wide = lz_type_widen(&left.type);
    LzType right_wide = lz_type_widen(&right.type);
    if (right.is_range) {
        /* `x in (a..b)` in a `for` header: the range is not a value. */
        error_range_here(parser, &right_at);
        return result;
    }

    if (check(parser, LZ_TOKEN_DOT_DOT) || check(parser, LZ_TOKEN_DOT_DOT_EQUAL)) {
        advance(parser);
        LzToken range_op = parser->previous;
        bool inclusive = range_op.type == LZ_TOKEN_DOT_DOT_EQUAL;
        skip_newlines(parser);
        LzToken end_at = parser->current;
        Expr end = parse_precedence(parser, PREC_BIT_OR);
        if (end.start.length != 0) {
            end_at = end.start;
        }
        LzType end_wide = lz_type_widen(&end.type);
        if (check(parser, LZ_TOKEN_BY)) {
            error_at_current(parser, "a range with 'by' cannot be used with 'in'");
            advance(parser);
            skip_newlines(parser);
            (void)parse_precedence(parser, PREC_BIT_OR); /* the step: parsed so no error follows */
            return result;
        }
        if ((lz_type_possible_kinds(&left_wide) & LZ_KINDS_NUMBER) == 0) {
            char text[64];
            char message[160];
            describe_bit_operand(&left.type, text, sizeof text);
            (void)snprintf(message, sizeof message,
                           "the value tested with a range must be a number, got %s", text);
            error_at(parser, &left.start, message);
        }
        if ((lz_type_possible_kinds(&right_wide) & LZ_KIND_INT) == 0) {
            error_range_bound(parser, &right_at, &right.type);
        }
        if ((lz_type_possible_kinds(&end_wide) & LZ_KIND_INT) == 0) {
            error_range_bound(parser, &end_at, &end.type);
        }
        emit_op_at(parser, LZ_OP_IN_RANGE, range_op.offset);
        emit_byte_at(parser, inclusive ? 1 : 0, range_op.offset);
        if (negate) {
            emit_op_at(parser, LZ_OP_NOT, op.offset);
        }
        return result;
    }

    if (check(parser, LZ_TOKEN_BY)) {
        /* `x in a by 2`: a range was meant, and it has no `..` */
        error_at_current(parser, "a range with 'by' cannot be used with 'in'");
        return result;
    }

    uint16_t kinds = lz_type_possible_kinds(&right_wide);
    if ((kinds & (LZ_KIND_ARRAY | LZ_KIND_DICT | LZ_KIND_STRING)) == 0) {
        char text[64];
        char message[160];
        describe_bit_operand(&right.type, text, sizeof text);
        (void)snprintf(message, sizeof message, "cannot use 'in' with %s", text);
        error_at(parser, &right_at, message);
    } else if (kinds == LZ_KIND_STRING &&
               (lz_type_possible_kinds(&left_wide) & LZ_KIND_STRING) == 0) {
        char text[64];
        char message[200];
        describe_bit_operand(&left.type, text, sizeof text);
        (void)snprintf(message, sizeof message,
                       "the left side of 'in' must be a string when the right side is a string, "
                       "got %s",
                       text);
        error_at(parser, &right_at, message); /* where the instruction reports it at run time */
    }
    emit_op_at(parser, LZ_OP_IN, right_at.offset);
    if (negate) {
        emit_op_at(parser, LZ_OP_NOT, op.offset);
    }
    return result;
}

/* The type with its nil taken out: what `a` can be when `a ?? b` did not use b. */
static LzType without_nil(const LzType *type) {
    LzType out = *type;
    out.kinds = (uint16_t)(out.kinds & ~LZ_KIND_NIL);
    return out;
}

/*
 * `a ?? b`: b only when a is nil (0, "", false and [] are values and stay). The lowest binary
 * operator, so `a or b ?? c` is `(a or b) ?? c`, and it chains to the left.
 *
 *     a  NIL_TEST  JUMP_IF_FALSE .keep  POP POP  b  JUMP .end  .keep: POP  .end:
 */
static Expr coalesce(Parser *parser, Expr left) {
    uint32_t offset = parser->previous.offset;
    emit_op_at(parser, LZ_OP_NIL_TEST, offset);
    size_t keep_jump = emit_jump(parser, LZ_OP_JUMP_IF_FALSE, offset);
    emit_op_at(parser, LZ_OP_POP, offset); /* the test */
    emit_op_at(parser, LZ_OP_POP, offset); /* the nil */
    skip_newlines(parser);
    if (rule_for(parser->current.type).prefix == NULL) {
        error_at_current(parser, "expected an expression after '?" "?'");
        return any_expr();
    }
    /* One level up: the caller's loop absorbs the next `??`, so a long chain iterates. */
    Expr right = parse_precedence(parser, PREC_OR);
    size_t end_jump = emit_jump(parser, LZ_OP_JUMP, offset);
    patch_jump(parser, keep_jump);
    emit_op_at(parser, LZ_OP_POP, offset); /* the test; the value stays */
    patch_jump(parser, end_jump);

    /* a without nil, joined with b. A left that cannot be nil is allowed all the same. */
    LzType kept = without_nil(&left.type);
    if (kept.kinds == 0 && kept.literal_count == 0) {
        return expr_of(right.type);
    }
    return union_expr(expr_of(kept), right);
}

/* `for` loops */

/*
 * The slots a `for` owns. They are plain locals without a name, so `break` and
 * `continue` cannot drop them by accident and the local count stays exact while the
 * body is compiled.
 */
#define FOR_SLOTS 4

static uint16_t slot_reserve(Parser *parser, uint32_t offset) {
    if (parser->local_count + FOR_SLOTS > LZ_MAX_LOCALS) {
        error_at_previous(parser, "too many local variables");
        return 0;
    }
    uint16_t base = (uint16_t)parser->local_count;
    for (int i = 0; i < FOR_SLOTS; i++) {
        emit_op_at(parser, LZ_OP_NIL, offset);
    }
    parser->local_count += FOR_SLOTS;
    return base;
}

static void slot_get(Parser *parser, uint16_t slot, uint32_t offset) {
    emit_op_at(parser, LZ_OP_GET_LOCAL, offset);
    emit_u16_at(parser, slot, offset);
}

static void slot_set(Parser *parser, uint16_t slot, uint32_t offset) {
    emit_op_at(parser, LZ_OP_SET_LOCAL, offset);
    emit_u16_at(parser, slot, offset);
}

static void slot_drop(Parser *parser, uint16_t base, uint32_t offset) {
    for (int i = 0; i < FOR_SLOTS; i++) {
        emit_op_at(parser, LZ_OP_POP, offset);
    }
    parser->local_count = base;
}

/* Is this type one a `for ... in` can walk, or can it not be told at compile time? */
static bool iterable_type(const LzType *type) {
    uint16_t kinds = lz_type_possible_kinds(type);
    return (kinds & (LZ_KIND_ARRAY | LZ_KIND_STRING | LZ_KIND_DICT)) != 0 || kinds == LZ_KINDS_ANY;
}

/* "cannot iterate over int": the kind, naming what the loop was given. */
static void error_iterate(Parser *parser, const LzToken *at, const LzType *type) {
    char text[64];
    char message[160];
    describe_bit_operand(type, text, sizeof text);
    (void)snprintf(message, sizeof message, "cannot iterate over %s", text);
    error_at(parser, at, message);
}

/* "range bounds must be ints, got string": the kind of the operand that was wrong. */
static void error_range_bound(Parser *parser, const LzToken *at, const LzType *type) {
    char text[64];
    char message[160];
    describe_bit_operand(type, text, sizeof text);
    (void)snprintf(message, sizeof message, "range bounds must be ints, got %s", text);
    error_at(parser, at, message);
}

/* The type of the values `for x in <iterable>` binds. */
static LzType loop_item_type(const LzType *iterable) {
    uint16_t kinds = lz_type_possible_kinds(iterable);
    if (kinds == LZ_KINDS_ANY) {
        return lz_type_of_kinds(LZ_KINDS_ANY);
    }
    if ((kinds & LZ_KIND_ARRAY) != 0) {
        return item_type_of(iterable);
    }
    if ((kinds & LZ_KIND_DICT) != 0) {
        /* `for k in d` walks the keys. */
        if ((kinds & (LZ_KIND_STRING | LZ_KIND_ARRAY)) == 0 && iterable->element != NULL) {
            return *iterable->element;
        }
        return lz_type_of_kinds(LZ_KINDS_ANY);
    }
    return lz_type_of_kinds(LZ_KIND_STRING); /* one code point */
}

/* The second name of `for a, b in x`: the value of a dict entry, otherwise the round. */
static bool pair_is_value(const LzType *iterable) {
    return (lz_type_possible_kinds(iterable) & LZ_KIND_DICT) != 0 &&
           (lz_type_possible_kinds(iterable) & (LZ_KIND_ARRAY | LZ_KIND_STRING)) == 0;
}

/*
 * A loop variable: a local that can be read but not assigned. Its slot is the value
 * about to be stored into it, as a `let` declaration's would be.
 */
static void loop_variable(Parser *parser, const LzToken *name_token, LzString *name,
                          const LzType *type) {
    Symbol *symbol = add_symbol(parser, name_token, name, SYMBOL_LOCAL, false, lz_value_nil(), type);
    if (symbol == NULL) {
        return;
    }
    symbol->loop_var = true;
    emit_op_at(parser, LZ_OP_NIL, name_token->offset); /* the slot; the store below fills it */
}

/*
 * `for x in xs`, `for i, x in xs`, `for ch: string in s`, and the ranges
 * `for i in a..b` / `a..=b` / `... by step`. The value after `in` is evaluated once;
 * a range becomes three hidden ints, anything else one hidden value plus a cursor.
 */
/*
 * Closes the upvalue of every captured loop variable of the round that just ended, so
 * a closure made in the body holds this round's value and the next round starts fresh.
 */
static void close_round_variables(Parser *parser, const LoopFrame *loop, uint32_t offset) {
    /*
     * Every variable of the loop's own scope is closed, because this runs before the body
     * is compiled for a C-style loop and so cannot know which ones a closure will capture.
     * Closing a variable nothing captured has no effect.
     */
    for (size_t i = parser->symbol_count; i > 0; i--) {
        const Symbol *symbol = &parser->symbols[i - 1];
        if (symbol->depth < loop->continue_depth) {
            break;
        }
        if (symbol->depth == loop->continue_depth && symbol->kind == SYMBOL_LOCAL) {
            emit_op_at(parser, LZ_OP_CLOSE_UPVALUE_KEEP, offset);
            emit_u16_at(parser, symbol->slot, offset);
        }
    }
}

/* "a range can only be used in a 'for' loop or after 'in'", with the caret on the operator. */
static void error_range_here(Parser *parser, const LzToken *at) {
    error_at(parser, at, "a range can only be used in a 'for' loop or after 'in'");
}

/*
 * `a..b` and `a..=b`, optionally `by step`. A range is not a value: it is only allowed
 * in the header of a `for`, where the three numbers become the loop's state. Anywhere
 * else it is an error at the operator.
 */
static Expr range_(Parser *parser, Expr left) {
    LzToken op = parser->previous;
    bool inclusive = op.type == LZ_TOKEN_DOT_DOT_EQUAL;
    if (!parser->for_range_ok) {
        /* Parse the rest so the statement ends cleanly, then report it. */
        error_range_here(parser, &op);
    }
    skip_newlines(parser);
    if ((lz_type_possible_kinds(&left.type) & LZ_KIND_INT) == 0) {
        error_range_bound(parser, &left.start, &left.type);
    }
    LzToken end_at = parser->current;
    Expr end = parse_precedence(parser, PREC_COMPARISON);
    if (end.start.length != 0) {
        end_at = end.start;
    }
    if ((lz_type_possible_kinds(&end.type) & LZ_KIND_INT) == 0) {
        error_range_bound(parser, &end_at, &end.type);
    }
    if (parser->range_case) {
        /* The item of a `case`: [a][b] is all there is, and a step has no meaning. */
        if (check(parser, LZ_TOKEN_BY)) {
            error_at_current(parser, "a range with 'by' cannot be used in a 'case'");
            advance(parser);
            skip_newlines(parser);
            (void)parse_precedence(parser, PREC_COMPARISON);
        }
    } else if (match(parser, LZ_TOKEN_BY)) {
        skip_newlines(parser);
        LzToken step_at = parser->current;
        Expr step = parse_precedence(parser, PREC_COMPARISON);
        if (step.start.length != 0) {
            step_at = step.start; /* the whole step, not just its first token */
        }
        if ((lz_type_possible_kinds(&step.type) & LZ_KIND_INT) == 0) {
            /* The step is not a bound, but the message names what was written. */
            error_range_bound(parser, &step_at, &step.type);
        }
        /* A written zero is caught here; any other step fails when the loop runs. */
        if (step_at.type == LZ_TOKEN_INTEGER && step_at.value.integer == 0) {
            error_at(parser, &step_at, "range step cannot be 0"); /* the caret goes on the step */
        }
    } else {
        emit_constant(parser, lz_value_int(1));
    }
    Expr range = expr_of(lz_type_of_kinds(LZ_KIND_INT));
    range.is_range = true;
    range.inclusive = inclusive;
    return range;
}

/* Patches every `continue` of the loop to the code that follows its body. */
static void patch_continues(Parser *parser, LoopFrame *loop) {
    for (size_t i = 0; i < loop->continue_count; i++) {
        patch_jump(parser, loop->continues[i]);
    }
}

static void for_in_statement(Parser *parser, uint32_t offset) {
    if (!check(parser, LZ_TOKEN_IDENTIFIER)) {
        error_at_current(parser, "expected a name for the loop variable");
        return;
    }
    advance(parser);
    LzToken first_token = parser->previous;
    LzString *first_name = intern_token(parser, &first_token);
    if (first_name == NULL) {
        return;
    }
    bool two = false;
    LzToken second_token = first_token;
    LzString *second_name = first_name;
    if (match(parser, LZ_TOKEN_COMMA)) {
        if (!check(parser, LZ_TOKEN_IDENTIFIER)) {
            error_at_current(parser, "expected a name for the loop variable");
            return;
        }
        advance(parser);
        second_token = parser->previous;
        second_name = intern_token(parser, &second_token);
        if (second_name == NULL) {
            return;
        }
        two = true;
    }

    LzType annotated = lz_type_of_kinds(LZ_KINDS_ANY);
    bool has_annotation = false;
    LzToken annotation_at = first_token;
    if (match(parser, LZ_TOKEN_COLON)) {
        has_annotation = true;
        annotation_at = parser->previous;
        (void)parse_type_union(parser, &annotated);
    }

    if (!match(parser, LZ_TOKEN_IN)) {
        error_at_current(parser,
                         "expected 'in' after the loop variable (a C-style loop starts with "
                         "'let': for let i = 0; ...)");
        return;
    }
    skip_newlines(parser);
    if (check(parser, LZ_TOKEN_LEFT_BRACE) || check(parser, LZ_TOKEN_EOF)) {
        error_at_current(parser, "expected an expression after 'in'");
        return;
    }

    uint16_t slots = slot_reserve(parser, offset);
    LzToken value_at = parser->current;
    parser->for_range_ok = true;
    int saved_no_in = parser->no_in_depth;
    parser->no_in_depth = parser->bracket_depth; /* the next `in` at this depth is not an operator */
    Expr start = expression(parser);
    parser->no_in_depth = saved_no_in;
    parser->for_range_ok = false;

    if (start.is_range) {
        /* The header left [start][end][step]: they become the loop's own slots. */
        bool inclusive = start.inclusive;
        slot_set(parser, (uint16_t)(slots + 2), offset); /* the step */
        slot_set(parser, (uint16_t)(slots + 1), offset); /* the limit, then the last value */
        slot_set(parser, (uint16_t)(slots + 0), offset); /* the counter */
        if (!check(parser, LZ_TOKEN_LEFT_BRACE)) {
            error_at_current(parser, "expected '{' after the 'for' header");
            return;
        }

        /*
         * The range is settled before the first round: an empty one (or one that runs
         * the wrong way) jumps past the loop, and the others get the last value they
         * visit, which is where stepping stops. The counter never leaves the int range.
         */
        slot_get(parser, (uint16_t)(slots + 0), offset);
        slot_get(parser, (uint16_t)(slots + 1), offset);
        slot_get(parser, (uint16_t)(slots + 2), offset);
        emit_op_at(parser, LZ_OP_RANGE_TEST, offset);
        emit_byte_at(parser, inclusive ? 1 : 0, offset);
        size_t empty_jump = emit_jump(parser, LZ_OP_JUMP_IF_FALSE, offset);
        emit_op_at(parser, LZ_OP_POP, offset);
        slot_get(parser, (uint16_t)(slots + 0), offset);
        slot_get(parser, (uint16_t)(slots + 1), offset);
        slot_get(parser, (uint16_t)(slots + 2), offset);
        emit_op_at(parser, LZ_OP_RANGE_LAST, offset);
        emit_byte_at(parser, inclusive ? 1 : 0, offset);
        slot_set(parser, (uint16_t)(slots + 1), offset);

        LoopFrame loop;
        memset(&loop, 0, sizeof loop);
        loop.enclosing = parser->loop;
        loop.func = parser->fs;
        loop.scope_depth = parser->scope_depth;
        loop.continue_depth = parser->scope_depth + 1;
        loop.forward_continues = true;
        parser->loop = &loop;

        size_t loop_start = parser->chunk->count;
        begin_scope(parser);
        loop_variable(parser, &first_token, first_name, &annotated);
        if (has_annotation) {
            LzType counter_type = lz_type_of_kinds(LZ_KIND_INT);
            check_static(parser, &counter_type, &annotated, &annotation_at);
        }
        slot_get(parser, (uint16_t)(slots + 0), offset);
        store_variable(parser, first_name, find_symbol(parser, first_name), &first_token);
        if (two) {
            /* `for i, x in ...` counts rounds; a range visits one value per round. */
            error_at(parser, &second_token, "a range binds one loop variable");
            loop_variable(parser, &second_token, second_name, &annotated);
        }

        block(parser);
        patch_continues(parser, &loop);
        close_round_variables(parser, &loop, offset);
        end_scope(parser);
        parser->loop = loop.enclosing;

        /* The last value ends the loop; anything before it steps and goes round again. */
        slot_get(parser, (uint16_t)(slots + 0), offset);
        slot_get(parser, (uint16_t)(slots + 1), offset);
        emit_op_at(parser, LZ_OP_EQUAL, offset);
        size_t done_jump = emit_jump(parser, LZ_OP_JUMP_IF_TRUE, offset);
        emit_op_at(parser, LZ_OP_POP, offset);
        slot_get(parser, (uint16_t)(slots + 0), offset);
        slot_get(parser, (uint16_t)(slots + 2), offset);
        emit_op_at(parser, LZ_OP_RANGE_NEXT, offset);
        slot_set(parser, (uint16_t)(slots + 0), offset);
        emit_loop(parser, loop_start, offset);

        /* Both ways out of the loop leave one test result on the stack: one pop serves both. */
        patch_jump(parser, done_jump);
        patch_jump(parser, empty_jump);
        emit_op_at(parser, LZ_OP_POP, offset);
        for (size_t i = 0; i < loop.break_count; i++) {
            patch_jump(parser, loop.breaks[i]);
        }
        free(loop.breaks);
        free(loop.continues);
        slot_drop(parser, slots, offset);
        return;
    }

    if (check(parser, LZ_TOKEN_BY)) {
        error_at_current(parser, "'by' only follows a range");
    }
    if (!iterable_type(&start.type)) {
        error_iterate(parser, &value_at, &start.type);
    }
    if (!check(parser, LZ_TOKEN_LEFT_BRACE)) {
        error_at_current(parser, "expected '{' after the 'for' header");
        return;
    }
    slot_set(parser, (uint16_t)(slots + 0), offset); /* the iterable */

    bool pair_value = two && pair_is_value(&start.type);
    LzType item = loop_item_type(&start.type);
    LzType first_type = lz_type_of_kinds(LZ_KIND_INT);
    if (pair_value) {
        /* `for k, v in d`: the first name is the key, the second the value. */
        first_type = item;
        item = dict_value_type(&start.type);
    }
    LzType item_static = item;
    if (has_annotation) {
        check_static(parser, &item, &annotated, &annotation_at);
        item = annotated;
    }
    /* An iterable that is not one is reported here, once, even if the loop never runs. */
    slot_get(parser, (uint16_t)(slots + 0), offset);
    emit_op_at(parser, LZ_OP_ITER_LENGTH, offset);
    slot_set(parser, (uint16_t)(slots + 3), offset); /* the size it began with, for a dict */
    emit_constant(parser, lz_value_int(0));
    slot_set(parser, (uint16_t)(slots + 1), offset); /* the cursor: an item index or a byte offset */
    emit_constant(parser, lz_value_int(0));
    slot_set(parser, (uint16_t)(slots + 2), offset); /* the code point index */

    LoopFrame loop;
    memset(&loop, 0, sizeof loop);
    loop.enclosing = parser->loop;
    loop.func = parser->fs;
    loop.scope_depth = parser->scope_depth;
    loop.continue_depth = parser->scope_depth + 1;
    loop.forward_continues = true;
    parser->loop = &loop;

    size_t loop_start = parser->chunk->count;
    /* A dict may have its values changed while it is walked, but not its size. */
    slot_get(parser, (uint16_t)(slots + 3), offset);
    slot_get(parser, (uint16_t)(slots + 0), offset);
    emit_op_at(parser, LZ_OP_ITER_CHECK, offset);
    /* The item count is read again every round, so an array that grows is walked further. */
    slot_get(parser, (uint16_t)(slots + 1), offset); /* the cursor */
    slot_get(parser, (uint16_t)(slots + 0), offset);
    emit_op_at(parser, LZ_OP_ITER_LENGTH, offset);
    emit_op_at(parser, LZ_OP_LESS, offset);
    size_t exit_jump = emit_jump(parser, LZ_OP_JUMP_IF_FALSE, offset);
    emit_op_at(parser, LZ_OP_POP, offset);

    begin_scope(parser);
    if (two) {
        loop_variable(parser, &first_token, first_name, &first_type);
    }
    loop_variable(parser, &second_token, second_name, &item);

    /* The item and the next cursor, with both hidden values still in their slots. */
    slot_get(parser, (uint16_t)(slots + 0), offset);
    slot_get(parser, (uint16_t)(slots + 1), offset);
    if (two) {
        /* [x][off] -> [x][first][second][next] */
        slot_get(parser, (uint16_t)(slots + 2), offset); /* the round, for an array */
        emit_op_at(parser, LZ_OP_ITER_PAIR, offset);
        slot_set(parser, (uint16_t)(slots + 1), offset); /* the next cursor, off the top */
        if (has_annotation) {
            emit_check_if_needed(parser, &item_static, &item, offset);
        }
        store_variable(parser, second_name, find_symbol(parser, second_name), &second_token);
        store_variable(parser, first_name, find_symbol(parser, first_name), &first_token);
        emit_op_at(parser, LZ_OP_POP, offset); /* the iterable copy */
    } else {
        emit_op_at(parser, LZ_OP_ITER_NEXT, offset);
        slot_set(parser, (uint16_t)(slots + 1), offset); /* the cursor moves past the item */
        if (has_annotation) {
            emit_check_if_needed(parser, &item_static, &item, offset);
        }
        store_variable(parser, second_name, find_symbol(parser, second_name), &second_token);
        /* The item is stored and the cursor is already in its slot: only the iterable copy is left. */
        emit_op_at(parser, LZ_OP_POP, offset);
    }

    block(parser);
    patch_continues(parser, &loop);
    close_round_variables(parser, &loop, offset); /* the item is fresh each round */
    end_scope(parser);
    parser->loop = loop.enclosing;

    if (two && !pair_value) {
        slot_get(parser, (uint16_t)(slots + 2), offset);
        emit_constant(parser, lz_value_int(1));
        emit_op_at(parser, LZ_OP_ADD, offset);
        slot_set(parser, (uint16_t)(slots + 2), offset);
    }
    emit_loop(parser, loop_start, offset);

    patch_jump(parser, exit_jump);
    emit_op_at(parser, LZ_OP_POP, offset);
    for (size_t i = 0; i < loop.break_count; i++) {
        patch_jump(parser, loop.breaks[i]);
    }
    free(loop.breaks);
    free(loop.continues);
    slot_drop(parser, slots, offset);
}

/* `let name [: type] [= value]` in a `for` header: no statement end, the `,` ends it. */
static void for_initializer(Parser *parser) {
    if (!check(parser, LZ_TOKEN_IDENTIFIER)) {
        error_at_current(parser, "expected variable name after 'let'");
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
    LzToken value_start = parser->current;
    Expr initializer;
    if (match(parser, LZ_TOKEN_EQUAL)) {
        value_start = parser->current;
        initializer = expression(parser);
    } else {
        emit_op_at(parser, LZ_OP_NIL, name_token.offset);
        initializer = expr_of(lz_type_of_kinds(LZ_KIND_NIL));
        if (annotated && lz_type_assignable(&initializer.type, &declared) == LZ_ASSIGN_NEVER) {
            error_types(parser, &name_token, "variable of type ", &declared,
                        " needs an initial value", NULL);
        }
    }
    if (annotated) {
        check_assignment(parser, &initializer.type, &declared, &value_start, name_token.offset);
    }
    LzType stored = annotated ? declared : lz_type_widen(&initializer.type);
    declare_variable(parser, &name_token, name, false, &stored);
}

/*
 * `for let i = 0; i < n; i += 1 { ... }`: the init runs once, then the condition,
 * the body and the step. The loop's variables are fresh for a closure made in the
 * body: their upvalues are closed before the step rewrites the slots.
 */
static void for_c_statement(Parser *parser, uint32_t offset) {
    begin_scope(parser);

    if (!check(parser, LZ_TOKEN_SEMICOLON)) {
        if (!match(parser, LZ_TOKEN_LET)) {
            error_at_current(parser, "the loop variable must be declared with 'let'");
            return;
        }
        for (;;) {
            parser->comma_list = true;
            for_initializer(parser);
            if (!match(parser, LZ_TOKEN_COMMA)) {
                break;
            }
            /* `let i = 0, j = n - 1`: the later declarations leave out the `let`. */
            (void)match(parser, LZ_TOKEN_LET);
            skip_newlines(parser);
        }
        parser->comma_list = false;
    }
    if (!match(parser, LZ_TOKEN_SEMICOLON)) {
        error_at_current(parser, "expected ';' after the loop initializer");
        return;
    }
    skip_newlines(parser);

    LoopFrame loop;
    memset(&loop, 0, sizeof loop);
    loop.enclosing = parser->loop;
    loop.func = parser->fs;
    /* `break` drops the loop's own variables; `continue` keeps them for the step. */
    loop.scope_depth = parser->scope_depth - 1;
    loop.continue_depth = parser->scope_depth;
    parser->loop = &loop;

    size_t loop_start = parser->chunk->count;
    size_t exit_jump;
    if (check(parser, LZ_TOKEN_SEMICOLON)) {
        emit_op_at(parser, LZ_OP_TRUE, offset);
        exit_jump = emit_jump(parser, LZ_OP_JUMP_IF_FALSE, offset);
        emit_op_at(parser, LZ_OP_POP, offset);
    } else {
        (void)expression(parser);
        exit_jump = emit_jump(parser, LZ_OP_JUMP_IF_FALSE, offset);
        emit_op_at(parser, LZ_OP_POP, offset);
    }
    if (!match(parser, LZ_TOKEN_SEMICOLON)) {
        error_at_current(parser, "expected ';' after the loop condition");
        parser->loop = loop.enclosing; /* `loop` is about to go out of scope */
        free(loop.breaks);
        free(loop.continues);
        return;
    }
    skip_newlines(parser);

    /*
     * The step runs after the body but is written before it in the source, so the first
     * round jumps over it; the end of the body and every `continue` enter it.
     */
    size_t over_step = emit_jump(parser, LZ_OP_JUMP, offset);
    size_t step_start = parser->chunk->count;
    loop.continue_target = step_start;
    /* A captured loop variable is closed here, so a closure holds this round's value. */
    close_round_variables(parser, &loop, offset);

    size_t steps = 0;
    while (!check(parser, LZ_TOKEN_LEFT_BRACE) && !check(parser, LZ_TOKEN_EOF)) {
        parser->comma_list = true;
        step_statement(parser);
        steps++;
        if (!match(parser, LZ_TOKEN_COMMA)) {
            break;
        }
        skip_newlines(parser);
    }
    parser->comma_list = false;
    (void)steps; /* `for (;;)` has no step */
    /* The step ends where the next round starts: back to the condition. */
    emit_loop(parser, loop_start, offset);

    patch_jump(parser, over_step);
    block(parser);
    /* The end of the body runs the step. */
    emit_loop(parser, step_start, offset);

    patch_jump(parser, exit_jump);
    emit_op_at(parser, LZ_OP_POP, offset);
    end_scope(parser);
    parser->loop = loop.enclosing;
    for (size_t i = 0; i < loop.break_count; i++) {
        patch_jump(parser, loop.breaks[i]);
    }
    free(loop.breaks);
    free(loop.continues);
}

static void for_statement(Parser *parser) {
    uint32_t offset = parser->previous.offset;
    if (check(parser, LZ_TOKEN_LET) || check(parser, LZ_TOKEN_SEMICOLON)) {
        for_c_statement(parser, offset);
    } else {
        for_in_statement(parser, offset);
    }
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
    /* `while true { ... }` has no way out but a return or a break, so nothing after it is reachable. */
    bool forever = check(parser, LZ_TOKEN_TRUE) && next_token_type(parser) == LZ_TOKEN_LEFT_BRACE;
    (void)expression(parser);
    if (!check(parser, LZ_TOKEN_LEFT_BRACE)) {
        error_at_current(parser, "expected '{' after condition");
        return;
    }

    size_t exit_jump = emit_jump(parser, LZ_OP_JUMP_IF_FALSE, offset);
    emit_op_at(parser, LZ_OP_POP, offset);

    LoopFrame loop;
    memset(&loop, 0, sizeof loop);
    loop.enclosing = parser->loop;
    loop.func = parser->fs;
    loop.continue_target = loop_start;
    loop.scope_depth = parser->scope_depth;
    loop.continue_depth = parser->scope_depth;
    parser->loop = &loop;

    block(parser);

    parser->loop = loop.enclosing;
    emit_loop(parser, loop_start, offset);
    patch_jump(parser, exit_jump);
    emit_op_at(parser, LZ_OP_POP, offset);
    for (size_t i = 0; i < loop.break_count; i++) {
        patch_jump(parser, loop.breaks[i]);
    }
    free(loop.breaks);
    parser->terminated = forever && loop.break_count == 0;
}

/*
 * `break` leaves the innermost loop and `continue` goes straight to its next
 * condition. Neither is reached from a function compiled inside the loop.
 */
static void loop_jump_statement(Parser *parser, bool is_break) {
    uint32_t offset = parser->previous.offset;
    LoopFrame *loop = parser->loop;

    if (loop == NULL || loop->func != parser->fs) {
        const char *control = is_break ? "break" : "continue";
        const char *why = loop == NULL ? "" : " (a function body is not part of the loop)";
        char message[96];
        (void)snprintf(message, sizeof message, "'%s' outside of a loop%s", control, why);
        error_at_previous(parser, message);
        end_statement(parser);
        return;
    }

    if (parser->guard.active && parser->guard.func == parser->fs && parser->guard.loop == loop) {
        char message[96];
        (void)snprintf(message, sizeof message, "cannot leave a 'finally' block with '%s'",
                       is_break ? "break" : "continue");
        error_at_previous(parser, message);
        end_statement(parser);
        return;
    }
    emit_try_exits(parser, true, offset);
    discard_loop_locals(parser, loop, offset, !is_break);
    if (is_break) {
        add_loop_break(parser, loop, emit_jump(parser, LZ_OP_JUMP, offset));
    } else if (loop->forward_continues) {
        /* The step of a `for` comes after the body, so the jump is patched once it is written. */
        add_loop_continue(parser, loop, emit_jump(parser, LZ_OP_JUMP, offset));
    } else {
        /* The same back-edge as the end of the loop: fuel and hot code behave alike. */
        emit_loop(parser, loop->continue_target, offset);
    }
    end_statement(parser);
    parser->terminated = true;
}

/* `switch` */

/* A literal case item seen so far, to catch the same one written twice. */
typedef struct {
    LzTokenType type;
    int64_t integer;
    uint32_t offset; /* of the lexeme, for a string */
    uint32_t length;
} CaseLiteral;

typedef struct {
    bool as_expr;
    bool has_subject;
    uint16_t slot;      /* the hidden local that holds the subject (a statement) */
    LzType subject;     /* its static type */
    CaseLiteral *seen;  /* the literal items so far */
    size_t seen_count;
    size_t seen_capacity;
    size_t *hits;       /* the jumps of one case's items that matched, to its body */
    size_t hit_count;
    size_t hit_capacity;
} SwitchState;

/* Skips to just past the `}` that closes the switch whose `{` took the depth to `depth`. */
static void switch_recover(Parser *parser, int depth) {
    while (!check(parser, LZ_TOKEN_EOF) && parser->bracket_depth >= depth) {
        advance(parser);
    }
}

static bool switch_add_hit(Parser *parser, SwitchState *state, size_t operand) {
    if (state->hit_count == state->hit_capacity) {
        size_t capacity = state->hit_capacity != 0 ? state->hit_capacity * 2 : 4;
        size_t *grown = realloc(state->hits, capacity * sizeof *grown);
        if (grown == NULL) {
            fail(parser, LZ_ERR_MEMORY);
            return false;
        }
        state->hits = grown;
        state->hit_capacity = capacity;
    }
    state->hits[state->hit_count++] = operand;
    return true;
}

/*
 * If the item just read was one literal (a number, a string, true, false or nil), checks it
 * against the ones before it in this switch: the same one twice can never be reached.
 */
static void switch_check_duplicate(Parser *parser, SwitchState *state, const LzToken *first) {
    const LzToken *last = &parser->previous;
    CaseLiteral literal;
    memset(&literal, 0, sizeof literal);
    LzToken at = *first;
    bool negative = first->type == LZ_TOKEN_MINUS && last->type == LZ_TOKEN_INTEGER &&
                    last->offset == first->offset + first->length;
    if (negative) {
        literal.type = LZ_TOKEN_INTEGER;
        literal.integer = (int64_t)(0 - (uint64_t)last->value.integer);
    } else if (first->offset == last->offset &&
               (first->type == LZ_TOKEN_INTEGER || first->type == LZ_TOKEN_STRING ||
                first->type == LZ_TOKEN_TRUE || first->type == LZ_TOKEN_FALSE ||
                first->type == LZ_TOKEN_NIL)) {
        literal.type = first->type;
        literal.integer = first->type == LZ_TOKEN_INTEGER ? first->value.integer : 0;
        literal.offset = first->offset;
        literal.length = first->length;
    } else {
        return;
    }
    for (size_t i = 0; i < state->seen_count; i++) {
        const CaseLiteral *other = &state->seen[i];
        bool same = other->type == literal.type;
        if (same && literal.type == LZ_TOKEN_INTEGER) {
            same = other->integer == literal.integer;
        } else if (same && literal.type == LZ_TOKEN_STRING) {
            same = other->length == literal.length &&
                   memcmp(parser->source->text + other->offset,
                          parser->source->text + literal.offset, literal.length) == 0;
        }
        if (same) {
            char message[160];
            int shown = (int)(negative ? last->offset + last->length - first->offset : first->length);
            (void)snprintf(message, sizeof message, "duplicate case %.*s", shown,
                           parser->source->text + first->offset);
            error_at(parser, &at, message);
            return;
        }
    }
    if (state->seen_count == state->seen_capacity) {
        size_t capacity = state->seen_capacity != 0 ? state->seen_capacity * 2 : 8;
        CaseLiteral *grown = realloc(state->seen, capacity * sizeof *grown);
        if (grown == NULL) {
            fail(parser, LZ_ERR_MEMORY);
            return;
        }
        state->seen = grown;
        state->seen_capacity = capacity;
    }
    state->seen[state->seen_count++] = literal;
}

/* Can a value of `subject` ever be == a value of `item`? An int and a float can be. */
static bool switch_kinds_meet(const LzType *subject, const LzType *item) {
    uint16_t a = lz_type_possible_kinds(subject);
    uint16_t b = lz_type_possible_kinds(item);
    if ((a & LZ_KINDS_NUMBER) != 0) {
        a = (uint16_t)(a | LZ_KINDS_NUMBER);
    }
    if ((b & LZ_KINDS_NUMBER) != 0) {
        b = (uint16_t)(b | LZ_KINDS_NUMBER);
    }
    return (a & b) != 0;
}

static void error_never_matches(Parser *parser, const LzToken *at, const LzType *subject,
                                const LzType *item) {
    char subject_text[64];
    char item_text[64];
    char message[220];
    describe_bit_operand(subject, subject_text, sizeof subject_text);
    describe_bit_operand(item, item_text, sizeof item_text);
    (void)snprintf(message, sizeof message,
                   "this case can never match: the subject is %s, but the case is %s", subject_text,
                   item_text);
    error_at(parser, at, message);
}

/*
 * One item of a `case`. With a subject it leaves whether the subject matches: a copy of the
 * subject is taken first, then the item is compared to it with == or, for a range, tested
 * against it. Without a subject the item is the condition itself.
 */
static void switch_item(Parser *parser, SwitchState *state) {
    LzToken first = parser->current;
    uint32_t offset = first.offset;
    if (state->has_subject) {
        if (state->as_expr) {
            emit_op_at(parser, LZ_OP_DUP, offset);
        } else {
            slot_get(parser, state->slot, offset);
        }
    }
    bool saved_ok = parser->for_range_ok;
    bool saved_case = parser->range_case;
    parser->for_range_ok = true;
    parser->range_case = true;
    Expr item = parse_precedence(parser, PREC_COALESCE);
    parser->for_range_ok = saved_ok;
    parser->range_case = saved_case;
    if (item.is_range) {
        if (!state->has_subject) {
            error_at(parser, &first, "a range cannot be used in a 'switch' without a subject");
            return;
        }
        LzType bounds = lz_type_of_kinds(LZ_KIND_INT);
        LzType subject = lz_type_widen(&state->subject);
        if ((lz_type_possible_kinds(&subject) & LZ_KINDS_NUMBER) == 0) {
            error_never_matches(parser, &first, &subject, &bounds);
        }
        emit_op_at(parser, LZ_OP_IN_RANGE, offset);
        emit_byte_at(parser, (uint8_t)((item.inclusive ? 1 : 0) | 2), offset);
        return;
    }
    if (!state->has_subject) {
        return;
    }
    LzType subject = lz_type_widen(&state->subject);
    LzType item_type = lz_type_widen(&item.type);
    if (!switch_kinds_meet(&subject, &item_type)) {
        error_never_matches(parser, &first, &subject, &item_type);
    } else {
        switch_check_duplicate(parser, state, &first);
    }
    emit_op_at(parser, LZ_OP_EQUAL, offset);
}

/*
 * Both forms. The subject is evaluated once. A statement keeps it in a hidden local of its
 * own scope, so `break`, `continue` and the end of the scope drop it like any other local.
 * An expression cannot name a slot (values of the expression it sits in may be below), so it
 * keeps the subject on the stack and copies it with OP_DUP; the value of the arm taken ends up
 * above it, and a swap and a pop leave the value alone.
 *
 *     case a, b { body }  =>   test a  JUMP_IF_TRUE .hit  POP  test b  JUMP_IF_FALSE .miss
 *                              .hit: POP  body  JUMP .end   .miss: POP
 */
static Expr switch_core(Parser *parser, bool as_expr) {
    LzToken switch_token = parser->previous;
    uint32_t offset = switch_token.offset;
    SwitchState state;
    memset(&state, 0, sizeof state);
    state.as_expr = as_expr;
    Expr result = any_expr();
    bool first_arm = true;
    bool has_else = false;
    bool all_terminate = true;
    size_t *end_jumps = NULL;
    size_t end_count = 0;
    size_t end_capacity = 0;
    int depth = 0;

    if (parser->nesting >= LZ_MAX_NESTING) {
        error_at_current(parser, "blocks nested too deeply");
        return result;
    }
    parser->nesting++;
    begin_scope(parser);

    if (!check(parser, LZ_TOKEN_LEFT_BRACE)) {
        if (check(parser, LZ_TOKEN_NEWLINE) || check(parser, LZ_TOKEN_EOF)) {
            error_at_current(parser, "expected an expression or '{' after 'switch'");
            goto done;
        }
        Expr subject = expression(parser);
        state.has_subject = true;
        state.subject = subject.type;
        if (!as_expr) {
            LzString *name = lz_vm_intern(parser->vm, "switch", 6);
            if (name == NULL) {
                fail(parser, lz_vm_out_of_memory(parser->vm));
                goto done;
            }
            LzType any = lz_type_of_kinds(LZ_KINDS_ANY);
            Symbol *hidden = add_symbol(parser, &switch_token, name, SYMBOL_LOCAL, false,
                                        lz_value_nil(), &any);
            if (hidden == NULL) {
                goto done;
            }
            state.slot = hidden->slot;
        }
        if (!check(parser, LZ_TOKEN_LEFT_BRACE)) {
            error_at_current(parser, "expected '{' after the 'switch' subject");
            goto done;
        }
    }
    advance(parser); /* the '{' */
    depth = parser->bracket_depth;
    skip_newlines(parser);

    while (!check(parser, LZ_TOKEN_RIGHT_BRACE) && !check(parser, LZ_TOKEN_EOF) &&
           parser->fatal == LZ_OK) {
        bool is_else = check(parser, LZ_TOKEN_ELSE);
        if (!is_else && !check(parser, LZ_TOKEN_CASE)) {
            error_at_current(parser, "expected 'case' or 'else' in a 'switch'");
            switch_recover(parser, depth);
            goto done;
        }
        if (has_else) {
            error_at_current(parser, "'else' must be the last arm");
            switch_recover(parser, depth);
            goto done;
        }
        advance(parser); /* `case` or `else` */
        uint32_t arm_offset = parser->previous.offset;
        size_t miss_jump = 0;
        bool has_miss = false;

        if (!is_else) {
            state.hit_count = 0;
            if (rule_for(parser->current.type).prefix == NULL) {
                error_at_current(parser, "expected an expression after 'case'");
                switch_recover(parser, depth);
                goto done;
            }
            for (;;) {
                switch_item(parser, &state);
                if (!match(parser, LZ_TOKEN_COMMA)) {
                    break;
                }
                skip_newlines(parser);
                if (rule_for(parser->current.type).prefix == NULL) {
                    error_at_current(parser, "expected an expression after 'case'");
                    switch_recover(parser, depth);
                    goto done;
                }
                if (!switch_add_hit(parser, &state,
                                    emit_jump(parser, LZ_OP_JUMP_IF_TRUE, arm_offset))) {
                    goto done;
                }
                emit_op_at(parser, LZ_OP_POP, arm_offset);
            }
            miss_jump = emit_jump(parser, LZ_OP_JUMP_IF_FALSE, arm_offset);
            has_miss = true;
            for (size_t i = 0; i < state.hit_count; i++) {
                patch_jump(parser, state.hits[i]);
            }
            emit_op_at(parser, LZ_OP_POP, arm_offset);
        }

        /* The body: a block, or `=>` and one expression. */
        if (check(parser, LZ_TOKEN_LEFT_BRACE)) {
            if (as_expr) {
                error_at_current(parser, "an arm of a 'switch' expression must use '=>'");
                switch_recover(parser, depth);
                goto done;
            }
            block(parser);
            all_terminate = all_terminate && parser->terminated;
        } else if (match(parser, LZ_TOKEN_FAT_ARROW)) {
            skip_newlines(parser);
            if (rule_for(parser->current.type).prefix == NULL) {
                error_at_current(parser, "expected an expression after '=>'");
                switch_recover(parser, depth);
                goto done;
            }
            Expr value = expression(parser);
            if (as_expr) {
                result = first_arm ? value : union_expr(result, value);
            } else {
                emit_op_at(parser, LZ_OP_POP, arm_offset);
                parser->terminated = false;
                all_terminate = false;
            }
        } else {
            error_at_current(parser, "expected '{' or '=>' after the case");
            switch_recover(parser, depth);
            goto done;
        }
        first_arm = false;

        if (is_else) {
            has_else = true;
        } else {
            if (end_count == end_capacity) {
                size_t capacity = end_capacity != 0 ? end_capacity * 2 : 4;
                size_t *grown = realloc(end_jumps, capacity * sizeof *grown);
                if (grown == NULL) {
                    fail(parser, LZ_ERR_MEMORY);
                    goto done;
                }
                end_jumps = grown;
                end_capacity = capacity;
            }
            end_jumps[end_count++] = emit_jump(parser, LZ_OP_JUMP, arm_offset);
        }
        if (has_miss) {
            patch_jump(parser, miss_jump);
            emit_op_at(parser, LZ_OP_POP, arm_offset);
        }
        skip_newlines(parser);
    }

    if (!check(parser, LZ_TOKEN_RIGHT_BRACE)) {
        error_at_current(parser, "expected '}' at the end of the 'switch'");
        goto done;
    }
    advance(parser);
    if (as_expr && !has_else) {
        error_at(parser, &switch_token, "a 'switch' expression needs an 'else'");
    }

    for (size_t i = 0; i < end_count; i++) {
        patch_jump(parser, end_jumps[i]);
    }
    if (as_expr && state.has_subject) {
        /* [subject][value] -> [value]. The subject was never a local, so no symbol goes. */
        emit_op_at(parser, LZ_OP_SWAP, offset);
        emit_op_at(parser, LZ_OP_POP, offset);
    }

done:
    free(end_jumps);
    free(state.seen);
    free(state.hits);
    parser->nesting--;
    end_scope(parser);
    if (!as_expr) {
        parser->terminated = has_else && all_terminate;
    }
    return as_expr ? result : any_expr();
}

static Expr switch_expression(Parser *parser) {
    return switch_core(parser, true);
}

static void switch_statement(Parser *parser) {
    (void)switch_core(parser, false);
}


/* `try`, `catch`, `finally`, `throw` */

static bool word_is(const Parser *parser, const LzToken *token, const char *word) {
    size_t length = strlen(word);
    return token->type == LZ_TOKEN_IDENTIFIER && token->length == length &&
           memcmp(parser->source->text + token->offset, word, length) == 0;
}

/*
 * One more copy of the finally body, compiled where a way out of the try runs it: the body is
 * read again from its place in the source. Names resolve as they did where it was written, so
 * the variables declared inside the try since then are hidden while it is compiled; locals of
 * its own go above whatever is on the stack now.
 */
static void emit_finally_copy(Parser *parser, TryContext *ctx) {
    if (ctx->failed || parser->fatal != LZ_OK || parser->panic) {
        return;
    }
    size_t errors_before = parser->diagnostics->count;

    size_t hidden_count = parser->symbol_count > ctx->symbol_start
                              ? parser->symbol_count - ctx->symbol_start
                              : 0;
    Symbol *hidden = NULL;
    if (hidden_count > 0) {
        hidden = malloc(hidden_count * sizeof *hidden);
        if (hidden == NULL) {
            fail(parser, LZ_ERR_MEMORY);
            return;
        }
        memcpy(hidden, &parser->symbols[ctx->symbol_start], hidden_count * sizeof *hidden);
        parser->symbol_count = ctx->symbol_start;
    }

    LzLexer saved_lexer = parser->lexer;
    LzToken saved_current = parser->current;
    LzToken saved_previous = parser->previous;
    int saved_bracket = parser->bracket_depth;
    int saved_no_in = parser->no_in_depth;
    bool saved_terminated = parser->terminated;
    TryContext *saved_try = parser->try_ctx;
    FinallyGuard saved_guard = parser->guard;

    parser->lexer = ctx->snapshot.lexer;
    parser->current = ctx->snapshot.current;
    parser->previous = ctx->snapshot.previous;
    parser->no_in_depth = -1;
    parser->try_ctx = ctx->enclosing; /* the tries outside this one still guard its finally body */
    parser->guard = (FinallyGuard){true, parser->fs, parser->loop};
    block(parser);

    parser->guard = saved_guard;
    parser->try_ctx = saved_try;
    parser->terminated = saved_terminated;
    parser->no_in_depth = saved_no_in;
    parser->bracket_depth = saved_bracket;
    parser->lexer = saved_lexer;
    parser->current = saved_current;
    parser->previous = saved_previous;
    if (hidden_count > 0) {
        memcpy(&parser->symbols[ctx->symbol_start], hidden, hidden_count * sizeof *hidden);
        parser->symbol_count = ctx->symbol_start + hidden_count;
        free(hidden);
    }
    if (parser->diagnostics->count > errors_before) {
        ctx->failed = true; /* the same mistake once is enough */
    }
}

/*
 * A `return`, `break` or `continue` leaves the tries around it, innermost first: each handler is
 * closed, and a finally body runs. loop_only: only the tries inside the innermost loop.
 */
static void emit_try_exits(Parser *parser, bool loop_only, uint32_t offset) {
    TryContext *start = parser->try_ctx;
    for (TryContext *ctx = start; ctx != NULL && ctx->func == parser->fs; ctx = ctx->enclosing) {
        if (loop_only && ctx->loop != parser->loop) {
            break;
        }
        emit_op_at(parser, LZ_OP_TRY_END, offset);
        if (ctx->has_finally) {
            emit_finally_copy(parser, ctx);
        }
    }
    parser->try_ctx = start;
}

typedef struct {
    bool has_catch;
    bool has_finally;
    FinallySnapshot finally_at;
} TryScan;

/* Skips a block whose `{` is *tok; leaves *tok on the token after its `}`. */
static bool scan_block(LzLexer *lexer, LzToken *tok) {
    int depth = 0;
    for (;;) {
        if (tok->type == LZ_TOKEN_LEFT_BRACE) {
            depth++;
        } else if (tok->type == LZ_TOKEN_RIGHT_BRACE) {
            depth--;
            if (depth == 0) {
                *tok = lz_lexer_next(lexer);
                return true;
            }
        } else if (tok->type == LZ_TOKEN_EOF || tok->type == LZ_TOKEN_ERROR) {
            return false;
        }
        *tok = lz_lexer_next(lexer);
    }
}

/*
 * Looks ahead from the `{` of the try block for its catch clauses and its finally body, so that
 * a `return` in the try can compile the finally body before the parser has reached it.
 */
static TryScan scan_try(const Parser *parser) {
    TryScan scan;
    memset(&scan, 0, sizeof scan);
    LzLexer lexer = parser->lexer;
    LzToken tok = parser->current;
    if (!scan_block(&lexer, &tok)) {
        return scan;
    }
    for (;;) {
        while (tok.type == LZ_TOKEN_NEWLINE) {
            tok = lz_lexer_next(&lexer);
        }
        if (tok.type == LZ_TOKEN_CATCH) {
            scan.has_catch = true;
            while (tok.type != LZ_TOKEN_LEFT_BRACE) {
                if (tok.type == LZ_TOKEN_EOF || tok.type == LZ_TOKEN_NEWLINE ||
                    tok.type == LZ_TOKEN_ERROR) {
                    return scan;
                }
                tok = lz_lexer_next(&lexer);
            }
            if (!scan_block(&lexer, &tok)) {
                return scan;
            }
            continue;
        }
        if (tok.type == LZ_TOKEN_FINALLY) {
            LzToken keyword = tok;
            tok = lz_lexer_next(&lexer);
            if (tok.type == LZ_TOKEN_LEFT_BRACE) {
                scan.has_finally = true;
                scan.finally_at.lexer = lexer;
                scan.finally_at.current = tok;
                scan.finally_at.previous = keyword;
            }
        }
        return scan;
    }
}

typedef struct {
    bool typed;      /* `catch Kind as ...` */
    uint8_t kind;    /* an LzErrorKind, or the number of a declared kind */
    bool has_name;   /* the error is bound to a variable */
    LzToken name;
    LzToken kind_token;
} CatchHeader;

/* After `catch`: what it catches and what the error is called. false after a reported mistake. */
static bool catch_header(Parser *parser, CatchHeader *header) {
    memset(header, 0, sizeof *header);
    if (check(parser, LZ_TOKEN_LEFT_BRACE)) {
        return true; /* catch { } */
    }
    if (!check(parser, LZ_TOKEN_IDENTIFIER)) {
        error_at_current(parser, "expected an error type, a name or '{' after 'catch'");
        return false;
    }
    LzToken first = parser->current;
    LzLexer ahead = parser->lexer;
    LzToken after = lz_lexer_next(&ahead);

    if (word_is(parser, &after, "as")) {
        header->typed = true;
        header->kind_token = first;
        if (!lz_vm_error_kind_from_name(parser->vm, parser->source->text + first.offset, first.length,
                                        &header->kind)) {
            LzSuggester suggester;
            lz_suggester_init(&suggester, parser->source->text + first.offset, first.length);
            size_t kind_count = lz_vm_error_kind_count(parser->vm);
            for (size_t i = 0; i < kind_count; i++) {
                lz_suggester_offer(&suggester, lz_vm_error_kind_name(parser->vm, (unsigned)i));
            }
            error_with_name_suggest(parser, &first, "unknown error type '", "'",
                                    "not a known error type", &suggester.found);
            header->kind = LZ_ERROR_ERROR;
        }
        advance(parser); /* the type */
        advance(parser); /* as */
        if (!check(parser, LZ_TOKEN_IDENTIFIER)) {
            error_at_current(parser, "expected a name after 'as'");
            return false;
        }
        advance(parser);
        header->name = parser->previous;
        header->has_name = !word_is(parser, &header->name, "_");
        return true;
    }

    uint8_t known;
    bool is_kind = lz_vm_error_kind_from_name(parser->vm, parser->source->text + first.offset,
                                              first.length, &known);
    if (after.type == LZ_TOKEN_LEFT_BRACE) {
        if (is_kind) {
            char message[300];
            char name[48];
            token_text(parser, &first, name, sizeof name);
            (void)snprintf(message, sizeof message,
                           "'catch %s { }' would call the error '%s'; to catch only that kind "
                           "write 'catch %s as _'",
                           name, name, name);
            error_at(parser, &first, message);
            return false;
        }
        advance(parser); /* catch e { }: the same as catch Error as e */
        header->name = parser->previous;
        header->has_name = true;
        return true;
    }
    if (is_kind) {
        error_at(parser, &after, "expected 'as' after the error type in 'catch'");
    } else {
        error_at(parser, &after, "expected '{' after the name in 'catch'");
    }
    return false;
}

static void try_statement(Parser *parser) {
    uint32_t offset = parser->previous.offset;
    LzToken try_token = parser->previous;
    if (!check(parser, LZ_TOKEN_LEFT_BRACE)) {
        error_at_current(parser, "expected '{' after 'try'");
        return;
    }
    TryScan scan = scan_try(parser);
    if (!scan.has_catch && !scan.has_finally) {
        error_at(parser, &try_token, "a 'try' needs a 'catch' or a 'finally'");
        /* Read the block anyway so that nothing else is reported for it. */
    }
    const size_t base = parser->local_count; /* the stack height of the statement */
    const size_t symbols_at_start = parser->symbol_count;
    size_t finally_jump = 0;
    size_t catch_jump = 0;
    TryContext finally_ctx;
    TryContext catch_ctx;
    memset(&finally_ctx, 0, sizeof finally_ctx);
    memset(&catch_ctx, 0, sizeof catch_ctx);

    if (scan.has_finally) {
        finally_jump = emit_jump(parser, LZ_OP_TRY_BEGIN, offset);
        finally_ctx = (TryContext){parser->try_ctx, parser->fs, parser->loop, true, false,
                                   symbols_at_start, scan.finally_at};
        parser->try_ctx = &finally_ctx;
    }
    if (scan.has_catch) {
        catch_jump = emit_jump(parser, LZ_OP_TRY_BEGIN, offset);
        catch_ctx = (TryContext){parser->try_ctx, parser->fs, parser->loop, false, false,
                                 symbols_at_start, scan.finally_at};
        parser->try_ctx = &catch_ctx;
    }

    block(parser); /* the body */

    uint8_t seen[LZ_ERROR_TYPES_MAX];
    size_t seen_count = 0;
    size_t *clause_ends = NULL;
    size_t clause_end_count = 0;
    size_t clause_end_capacity = 0;

    if (scan.has_catch) {
        parser->try_ctx = catch_ctx.enclosing;
        emit_op_at(parser, LZ_OP_TRY_END, offset);
        size_t body_done = emit_jump(parser, LZ_OP_JUMP, offset);
        patch_jump(parser, catch_jump); /* an error lands here with the error value on the stack */

        /* The error value is a local of its own: every clause reads it. */
        begin_scope(parser);
        LzString *hidden_name = lz_vm_intern(parser->vm, "(error)", 7);
        LzType error_type = lz_type_of_kinds(LZ_KIND_ERROR);
        Symbol *hidden = hidden_name == NULL
                             ? NULL
                             : add_symbol(parser, &try_token, hidden_name, SYMBOL_LOCAL, false,
                                          lz_value_nil(), &error_type);
        uint16_t error_slot = hidden != NULL ? hidden->slot : (uint16_t)base;
        bool catch_all = false;

        for (;;) {
            skip_newlines(parser);
            if (!check(parser, LZ_TOKEN_CATCH)) {
                break;
            }
            advance(parser);
            LzToken catch_token = parser->previous;
            uint32_t clause_offset = catch_token.offset;
            if (catch_all) {
                error_at(parser, &catch_token,
                         "a 'catch' after a 'catch' with no error type can never run");
            }
            CatchHeader header;
            bool ok = catch_header(parser, &header);
            if (!ok) {
                /* One mistake, one error: skip the clause. */
                while (!check(parser, LZ_TOKEN_LEFT_BRACE) && !check(parser, LZ_TOKEN_EOF) &&
                       !check(parser, LZ_TOKEN_NEWLINE)) {
                    advance(parser);
                }
            }
            if (ok && header.typed) {
                for (size_t i = 0; i < seen_count; i++) {
                    if (lz_vm_error_kind_is(parser->vm, header.kind, seen[i])) {
                        char message[200];
                        (void)snprintf(message, sizeof message,
                                       "unreachable catch: '%s' is already caught by '%s' above",
                                       lz_vm_error_kind_name(parser->vm, header.kind),
                                       lz_vm_error_kind_name(parser->vm, seen[i]));
                        error_at(parser, &header.kind_token, message);
                        break;
                    }
                }
                if (seen_count < LZ_ERROR_TYPES_MAX) {
                    seen[seen_count++] = header.kind;
                }
            } else if (ok) {
                catch_all = true;
            }
            if (!check(parser, LZ_TOKEN_LEFT_BRACE)) {
                if (ok) {
                    error_at_current(parser, "expected '{' to start the 'catch' block");
                }
                break;
            }

            size_t next_clause = 0;
            bool tests = ok && header.typed;
            if (tests) {
                slot_get(parser, error_slot, clause_offset);
                emit_op_at(parser, LZ_OP_ERROR_IS, clause_offset);
                emit_byte_at(parser, (uint8_t)header.kind, clause_offset);
                next_clause = emit_jump(parser, LZ_OP_JUMP_IF_FALSE, clause_offset);
                emit_op_at(parser, LZ_OP_POP, clause_offset);
            }
            begin_scope(parser);
            if (ok && header.has_name) {
                LzString *name = intern_token(parser, &header.name);
                slot_get(parser, error_slot, clause_offset);
                Symbol *variable =
                    name == NULL ? NULL
                                 : add_symbol(parser, &header.name, name, SYMBOL_LOCAL, false,
                                              lz_value_nil(), &error_type);
                if (variable != NULL) {
                    variable->catch_var = true;
                } else {
                    emit_op_at(parser, LZ_OP_POP, clause_offset);
                }
            }
            block(parser);
            end_scope(parser);
            if (clause_end_count == clause_end_capacity) {
                size_t capacity = clause_end_capacity != 0 ? clause_end_capacity * 2 : 4;
                size_t *grown = realloc(clause_ends, capacity * sizeof *grown);
                if (grown == NULL) {
                    fail(parser, LZ_ERR_MEMORY);
                    break;
                }
                clause_ends = grown;
                clause_end_capacity = capacity;
            }
            clause_ends[clause_end_count++] = emit_jump(parser, LZ_OP_JUMP, clause_offset);
            if (tests) {
                patch_jump(parser, next_clause);
                emit_op_at(parser, LZ_OP_POP, clause_offset);
            }
        }

        if (!catch_all) {
            /* No clause took it: it goes on to the tries around this one. */
            slot_get(parser, error_slot, offset);
            emit_op_at(parser, LZ_OP_THROW, offset);
        }
        for (size_t i = 0; i < clause_end_count; i++) {
            patch_jump(parser, clause_ends[i]);
        }
        free(clause_ends);
        end_scope(parser); /* the error value */
        patch_jump(parser, body_done);
    }

    if (scan.has_finally) {
        parser->try_ctx = finally_ctx.enclosing;
        emit_op_at(parser, LZ_OP_TRY_END, offset);
        skip_newlines(parser);
        if (match(parser, LZ_TOKEN_FINALLY)) {
            /* The way in with no error: the body, read for real. */
            FinallyGuard saved_guard = parser->guard;
            parser->guard = (FinallyGuard){true, parser->fs, parser->loop};
            if (check(parser, LZ_TOKEN_LEFT_BRACE)) {
                block(parser);
            } else {
                error_at_current(parser, "expected '{' after 'finally'");
            }
            parser->guard = saved_guard;
        }
        size_t normal_done = emit_jump(parser, LZ_OP_JUMP, offset);

        /* The way in with an error: the body again, then the error goes on. */
        patch_jump(parser, finally_jump);
        begin_scope(parser);
        LzString *hidden_name = lz_vm_intern(parser->vm, "(error)", 7);
        LzType error_type = lz_type_of_kinds(LZ_KIND_ERROR);
        Symbol *hidden = hidden_name == NULL
                             ? NULL
                             : add_symbol(parser, &try_token, hidden_name, SYMBOL_LOCAL, false,
                                          lz_value_nil(), &error_type);
        uint16_t error_slot = hidden != NULL ? hidden->slot : (uint16_t)base;
        finally_ctx.symbol_start = parser->symbol_count; /* the hidden error stays visible */
        emit_finally_copy(parser, &finally_ctx);
        slot_get(parser, error_slot, offset);
        emit_op_at(parser, LZ_OP_THROW, offset);
        end_scope(parser);
        patch_jump(parser, normal_done);
    }
    parser->try_ctx = scan.has_catch ? catch_ctx.enclosing : parser->try_ctx;
    if (scan.has_finally) {
        parser->try_ctx = finally_ctx.enclosing;
    }
    parser->terminated = false;
}

static void throw_statement(Parser *parser) {
    uint32_t offset = parser->previous.offset;
    if (check(parser, LZ_TOKEN_NEWLINE) || check(parser, LZ_TOKEN_SEMICOLON) ||
        check(parser, LZ_TOKEN_RIGHT_BRACE) || check(parser, LZ_TOKEN_EOF)) {
        error_at_current(parser, "expected an error to throw");
        return;
    }
    LzToken start = parser->current;
    Expr value = expression(parser);
    LzType wide = lz_type_widen(&value.type);
    if (value.native != NULL && !value.is_call && lz_native_error_kind(value.native, NULL)) {
        /* `throw NotFound`: the kind is a function that makes the error, it is not one. */
        char message[160];
        (void)snprintf(message, sizeof message,
                       "an error type must be called to make an error: %s(\"message\")",
                       value.native->name->data);
        error_at(parser, &start, message);
    } else if ((lz_type_possible_kinds(&wide) & LZ_KIND_ERROR) == 0) {
        error_at(parser, &start, "only an error can be thrown (write RuntimeError(\"oops\"))");
    }
    emit_op_at(parser, LZ_OP_THROW, offset);
    end_statement(parser);
    parser->terminated = true;
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
        case LZ_OP_BIT_AND:
        case LZ_OP_BIT_OR:
        case LZ_OP_BIT_XOR:
        case LZ_OP_BIT_NOT:
        case LZ_OP_SHL:
        case LZ_OP_SHR:
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
    bool list = parser->comma_list;
    parser->comma_list = false;
    if (list) {
        /* Inside a `for` header: the list, the ';' or the '{' is the header's business. */
        (void)match(parser, LZ_TOKEN_NEWLINE);
        return;
    }
    if (match(parser, LZ_TOKEN_NEWLINE) || match(parser, LZ_TOKEN_SEMICOLON)) {
        return;
    }
    if (check(parser, LZ_TOKEN_EOF) || check(parser, LZ_TOKEN_RIGHT_BRACE)) {
        return;
    }
    if (check(parser, LZ_TOKEN_EQUAL) || is_compound(parser->current.type)) {
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
    /* `let d = {...}` stays an `any` (a later `d = 5` is legal), but `d["a"]` reads a key
     * rather than an array index, which a string would not survive. */
    if (!annotated && has_initializer) {
        uint16_t kinds = lz_type_possible_kinds(&initializer.type);
        bool dict = kinds == LZ_KIND_DICT || initializer.dict_value;
        bool array = kinds == LZ_KIND_ARRAY || initializer.array_value;
        Symbol *symbol = find_symbol(parser, name);
        if (symbol != NULL && (dict || array)) {
            symbol->dict_value = dict;
            symbol->array_value = array;
        }
    }
}

/* The instruction that stores the top of the stack into a variable. */
static void store_variable(Parser *parser, LzString *name, const Symbol *symbol,
                           const LzToken *at) {
    if (symbol != NULL && symbol->kind == SYMBOL_LOCAL && symbol->owner != parser->fs) {
        int index = resolve_upvalue(parser, parser->fs, (Symbol *)symbol);
        emit_op_at(parser, LZ_OP_SET_UPVALUE, at->offset);
        emit_u16_at(parser, (uint16_t)(index < 0 ? 0 : index), at->offset);
    } else if (symbol != NULL && symbol->kind == SYMBOL_LOCAL) {
        emit_op_at(parser, LZ_OP_SET_LOCAL, at->offset);
        emit_u16_at(parser, symbol->slot, at->offset);
    } else {
        emit_named(parser, LZ_OP_SET_GLOBAL, name, at->offset);
    }
}

/* `name = value`, and the compound forms `name += value` and so on. */
static void assignment_statement(Parser *parser) {
    advance(parser);
    LzToken name_token = parser->previous;
    LzString *name = intern_token(parser, &name_token);
    LzToken op_token = parser->current;
    LzTokenType assign = op_token.type;
    bool compound = is_compound(assign);
    advance(parser);
    skip_newlines(parser);

    Symbol *symbol = name != NULL ? find_symbol(parser, name) : NULL;
    if (symbol != NULL && symbol->loop_var) {
        error_with_name(parser, &name_token, "cannot assign to the loop variable '", "'");
        return;
    }
    if (symbol != NULL && symbol->catch_var) {
        error_with_name(parser, &name_token, "cannot assign to the catch variable '", "'");
        return;
    }
    bool is_constant = symbol != NULL && (symbol->is_const || symbol->kind == SYMBOL_CONSTANT);
    if (compound && name != NULL) {
        if (symbol == NULL && lz_vm_lookup(parser->vm, name) != NULL) {
            error_with_name(parser, &name_token, "cannot assign to builtin '", "'");
        } else if (is_constant) {
            error_with_name(parser, &name_token, "cannot assign to constant '", "'");
        } else {
            /*
             * The variable is read and computed on the stack, so it is evaluated once.
             * ponytail: `x += 1` on a global that is not defined yet reads it before
             * writing, so it fails like `x = x + 1` does. Nothing else does.
             */
            Expr target = variable_at(parser, &name_token);
            skip_newlines(parser);
            LzToken value_start = parser->current;
            Expr value = expression(parser);
            Expr result = compound_value(parser, &op_token, &target, &value);
            emit_op_at(parser, compound_opcode(compound_operator(assign)), name_token.offset);
            /* An undefined global is checked when the store runs, like `=` does. */
            check_assignment(parser, &result.type, symbol != NULL ? &symbol->type : &target.type,
                             &value_start, name_token.offset);
            store_variable(parser, name, symbol, &name_token);
        }
        end_statement(parser);
        return;
    }

    LzToken value_start = parser->current;
    Expr value = expression(parser);
    if (name == NULL) {
        return;
    }

    if (symbol == NULL) {
        if (lz_vm_lookup(parser->vm, name) != NULL) {
            error_with_name(parser, &name_token, "cannot assign to builtin '", "'");
        } else {
            /* Sequential execution: checked when the assignment runs (SET_GLOBAL). */
            record_name_hint(parser, &name_token, false, true, false);
            emit_named(parser, LZ_OP_SET_GLOBAL, name, name_token.offset);
        }
    } else if (is_constant) {
        error_with_name(parser, &name_token, "cannot assign to constant '", "'");
    } else {
        check_assignment(parser, &value.type, &symbol->type, &value_start, name_token.offset);
        store_variable(parser, name, symbol, &name_token);
        /* The variable may hold anything again: `x[k]` is settled by the value it gets. */
        ((Symbol *)symbol)->dict_value = false;
        ((Symbol *)symbol)->array_value = false;
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


/* Is this token a compound assignment operator (`+=`, `<<=`, ...)? */
static bool is_compound(LzTokenType type) {
    return type >= LZ_TOKEN_PLUS_EQUAL && type <= LZ_TOKEN_GREATER_GREATER_EQUAL;
}

/* The binary operator behind a compound assignment: `+=` is `+` on the old value. */
static LzTokenType compound_operator(LzTokenType assign) {
    switch (assign) {
    case LZ_TOKEN_PLUS_EQUAL:
        return LZ_TOKEN_PLUS;
    case LZ_TOKEN_MINUS_EQUAL:
        return LZ_TOKEN_MINUS;
    case LZ_TOKEN_STAR_EQUAL:
        return LZ_TOKEN_STAR;
    case LZ_TOKEN_SLASH_EQUAL:
        return LZ_TOKEN_SLASH;
    case LZ_TOKEN_SLASH_SLASH_EQUAL:
        return LZ_TOKEN_SLASH_SLASH;
    case LZ_TOKEN_PERCENT_EQUAL:
        return LZ_TOKEN_PERCENT;
    case LZ_TOKEN_AMPERSAND_EQUAL:
        return LZ_TOKEN_AMPERSAND;
    case LZ_TOKEN_PIPE_EQUAL:
        return LZ_TOKEN_PIPE;
    case LZ_TOKEN_CARET_EQUAL:
        return LZ_TOKEN_CARET;
    case LZ_TOKEN_LESS_LESS_EQUAL:
        return LZ_TOKEN_LESS_LESS;
    default:
        return LZ_TOKEN_GREATER_GREATER;
    }
}

/* The instruction that computes that operator on the two values on the stack. */
static LzOpCode compound_opcode(LzTokenType type) {
    switch (type) {
    case LZ_TOKEN_PLUS:
        return LZ_OP_ADD;
    case LZ_TOKEN_MINUS:
        return LZ_OP_SUBTRACT;
    case LZ_TOKEN_STAR:
        return LZ_OP_MULTIPLY;
    case LZ_TOKEN_SLASH:
        return LZ_OP_DIVIDE;
    case LZ_TOKEN_SLASH_SLASH:
        return LZ_OP_FLOOR_DIVIDE;
    case LZ_TOKEN_PERCENT:
        return LZ_OP_MODULO;
    case LZ_TOKEN_AMPERSAND:
        return LZ_OP_BIT_AND;
    case LZ_TOKEN_PIPE:
        return LZ_OP_BIT_OR;
    case LZ_TOKEN_CARET:
        return LZ_OP_BIT_XOR;
    case LZ_TOKEN_LESS_LESS:
        return LZ_OP_SHL;
    default:
        return LZ_OP_SHR;
    }
}

/*
 * The type of `target op value`, checked the way the binary expression would check
 * it. Nothing is emitted: both operands are already on the stack. `at` is the `+=`
 * token; it keeps its position and text for the message.
 */
static Expr compound_value(Parser *parser, const LzToken *at, const Expr *target,
                           const Expr *value) {
    LzToken op = *at;
    LzTokenType type = compound_operator(at->type);
    op.type = type;
    switch (type) {
    case LZ_TOKEN_AMPERSAND:
    case LZ_TOKEN_PIPE:
    case LZ_TOKEN_CARET:
    case LZ_TOKEN_LESS_LESS:
    case LZ_TOKEN_GREATER_GREATER:
        if ((lz_type_possible_kinds(&target->type) & LZ_KIND_INT) == 0 ||
            (lz_type_possible_kinds(&value->type) & LZ_KIND_INT) == 0) {
            error_bitwise(parser, &op, &target->type, &value->type);
        }
        return expr_of(lz_type_of_kinds(LZ_KIND_INT));
    default:
        return arithmetic_result(parser, &op, target, value);
    }
}

static LzTokenType next_token_type(const Parser *parser) {
    LzLexer ahead = parser->lexer;
    return lz_lexer_next(&ahead).type;
}

/*
 * One statement of a `for` step: a call, `i = ...` or `i += ...`. Anything else is
 * not an assignment or a call. A step never ends the statement: the `,` or the `{`
 * after it does.
 */
static void step_statement(Parser *parser) {
    LzTokenType type = parser->current.type;
    if (type == LZ_TOKEN_IDENTIFIER) {
        LzTokenType after = next_token_type(parser);
        if (after == LZ_TOKEN_EQUAL || is_compound(after)) {
            assignment_statement(parser);
            return;
        }
    } else if (type == LZ_TOKEN_FN || type == LZ_TOKEN_LET || type == LZ_TOKEN_CONST ||
               type == LZ_TOKEN_IF || type == LZ_TOKEN_SWITCH || type == LZ_TOKEN_TRY ||
               type == LZ_TOKEN_THROW || type == LZ_TOKEN_WHILE ||
               type == LZ_TOKEN_FOR ||
               type == LZ_TOKEN_RETURN || type == LZ_TOKEN_BREAK ||
               type == LZ_TOKEN_CONTINUE || type == LZ_TOKEN_LEFT_BRACE ||
               type == LZ_TOKEN_RIGHT_BRACE || type == LZ_TOKEN_EOF ||
               type == LZ_TOKEN_SEMICOLON) {
        error_at_current(parser, "expected an assignment or a call in the loop step");
        return;
    }
    LzToken start = parser->current;
    Expr expr = expression(parser);
    if (member_assignment(parser, &expr, false)) {
        return;
    }
    if (!expr.is_call) {
        error_at(parser, &start, "expected an assignment or a call in the loop step");
        return;
    }
    emit_op_at(parser, LZ_OP_POP, start.offset);
}

static void expression_statement(Parser *parser) {
    uint32_t offset = parser->current.offset;
    Expr target = expression(parser);

    /* a[i] = v: the expression just compiled ends in OP_INDEX, which becomes OP_SET_INDEX. */
    bool plain = check(parser, LZ_TOKEN_EQUAL);
    bool compound = is_compound(parser->current.type);
    if ((plain || compound) && target.is_index && parser->fatal == LZ_OK &&
        parser->chunk->count == target.op_pos + 1) {
        uint32_t index_offset = parser->chunk->offsets[target.op_pos];
        if (target.base_member && parser->chunk->code[target.base_pos] == LZ_OP_GET_FIELD) {
            parser->chunk->code[target.base_pos] = LZ_OP_GET_FIELD_W; /* the container is changed in place */
        }
        LzToken op_token = parser->current;
        LzTokenType assign = op_token.type;
        advance(parser);
        skip_newlines(parser);
        LzToken value_start = parser->current;
        if (compound) {
            /*
             * a[i] += v: OP_INDEX_KEEP left [array][index][item], so v goes on top and
             * the operator leaves the new item: exactly what OP_SET_INDEX takes. The
             * array and the index are evaluated once and stay put.
             */
            Expr value = expression(parser);
            Expr result = compound_value(parser, &op_token, &target, &value);
            emit_op_at(parser, compound_opcode(compound_operator(assign)), index_offset);
            check_assignment(parser, &result.type, &target.type, &value_start, index_offset);
            emit_op_at(parser, LZ_OP_SET_INDEX, index_offset);
            end_statement(parser);
            return;
        }

        lz_chunk_truncate(&parser->vm->heap, parser->chunk, target.op_pos,
                          parser->chunk->constant_count);
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

    if (member_assignment(parser, &target, true)) {
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
    bool is_member = parser->member_pending;
    bool is_static_member = parser->member_pending && parser->member_static;
    parser->member_pending = false;
    if (parser->class_name != NULL) {
        function->owner = parser->class_name; /* closures inside inherit it: "inside the class" is lexical */
        function->owner_id = parser->current_class_id;
        lz_object_retain(&function->owner->base);
    }

    FuncState fs;
    fs.static_member = is_static_member;
    fs.method_member = is_member && !is_static_member;
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
            bool is_self = is_member && param->length == 4 && memcmp(param->data, "self", 4) == 0;
            if (is_self && param_count != 0) {
                error_at(parser, &param_token, "'self' must be the first parameter of a method");
            } else if (is_self && !rest && parser->error_self_kind != 0) {
                declared = lz_type_of_kinds(LZ_KIND_ERROR); /* self is the error value */
                declared.class_id = parser->error_self_kind;
            } else if (is_self && !rest && parser->in_extend) {
                /* An extension of a builtin kind checks `self` against it; of a class, not at all. */
                declared = lz_type_of_kinds(parser->extend_kinds != 0 ? parser->extend_kinds : LZ_KINDS_ANY);
            } else if (is_self && !rest) {
                declared = lz_type_of_kinds(LZ_KIND_INSTANCE); /* Point.sum(x) checks x is a Point */
                declared.class_name = parser->class_name;
                declared.class_id = parser->current_class_id;
            }
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
        if (check(parser, LZ_TOKEN_LEFT_BRACE) && !arrow_starts_dict(parser)) {
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
    } else if (!returns && is_member &&
               (check(parser, LZ_TOKEN_NEWLINE) || check(parser, LZ_TOKEN_RIGHT_BRACE) ||
                check(parser, LZ_TOKEN_SEMICOLON) || check(parser, LZ_TOKEN_EOF))) {
        if (parser->in_trait) {
            parser->last_bodiless = true; /* a requirement: the trait says what a class must provide */
        } else {
            error_at_current(parser, "this function needs a body (only a trait can leave it out)");
        }
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
    if (fs.upvalue_count != 0 && is_member) {
        error_at_previous(parser, "a class member cannot use variables of an enclosing function");
    }
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


/* ---- Classes ---- */

static LzToken peek_token(const Parser *parser, int count) {
    LzLexer ahead = parser->lexer;
    LzToken token = lz_lexer_next(&ahead);
    for (int i = 1; i < count; i++) {
        token = lz_lexer_next(&ahead);
    }
    return token;
}

/* A function of no parameters that evaluates one expression: a field default or a constant. */
static bool compile_thunk(Parser *parser, LzString *name, uint32_t offset, LzValue *out) {
    LzHeap *heap = &parser->vm->heap;
    LzFunction *function = lz_function_new(heap, name);
    FuncSig *sig = function != NULL ? new_sig(parser, name) : NULL;
    if (function == NULL || sig == NULL) {
        if (function != NULL) {
            lz_object_release(heap, &function->base);
        } else {
            fail(parser, lz_vm_out_of_memory(parser->vm));
        }
        return false;
    }
    function->offset = offset;
    function->owner = parser->class_name;
    function->owner_id = parser->current_class_id;
    lz_object_retain(&function->owner->base);

    FuncState fs;
    memset(&fs, 0, sizeof fs);
    fs.enclosing = parser->fs;
    fs.function = function;
    fs.first_symbol = parser->symbol_count;
    fs.self_symbol = SIZE_MAX;
    fs.sig = sig;
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
    bool was_default = parser->in_default;
    parser->in_default = true;

    uint32_t at = parser->current.offset;
    (void)expression(parser);
    emit_op_at(parser, LZ_OP_RETURN, at);

    parser->in_default = was_default;
    while (parser->symbol_count > fs.first_symbol) {
        Symbol *symbol = &parser->symbols[--parser->symbol_count];
        if (symbol->kind == SYMBOL_CONSTANT) {
            lz_value_release(heap, &symbol->value);
        }
    }
    parser->scope_depth--;
    parser->nesting--;
    if (!lz_function_set_params(function, NULL, 0, 0, false) ||
        !lz_function_set_upvalues(function, fs.upvalues, fs.upvalue_count)) {
        fail(parser, LZ_ERR_MEMORY);
    }
    free(parser->constants.slots);
    parser->chunk = fs.saved_chunk;
    parser->constants = fs.saved_constants;
    parser->local_count = fs.saved_local_count;
    parser->fs = fs.enclosing;
    parser->terminated = false;

    bool ok = false;
    LzClosure *closure = lz_closure_new(heap, function);
    if (closure == NULL) {
        fail(parser, lz_vm_out_of_memory(parser->vm));
    } else {
        *out = lz_value_object(&closure->base); /* the creation reference moves to the caller */
        ok = true;
    }
    lz_object_release(heap, &function->base);
    return ok;
}

static const char *const unsupported_member_words[] = {"trait", "extend", "super"};

static bool is_unsupported_word(const Parser *parser, const LzToken *token) {
    for (size_t i = 0; i < sizeof unsupported_member_words / sizeof *unsupported_member_words; i++) {
        if (word_is(parser, token, unsupported_member_words[i])) {
            return true;
        }
    }
    return false;
}

/* Makes the closure the function just compiled became (the one constant it left in the code). */
static bool take_member_closure(Parser *parser, LzValue *out) {
    LzChunk *chunk = parser->chunk;
    if (parser->panic || parser->fatal != LZ_OK) {
        return false;
    }
    if (chunk->count < 3 || chunk->code[chunk->count - 3] != LZ_OP_CONSTANT) {
        return false;
    }
    uint16_t index = (uint16_t)((chunk->code[chunk->count - 2] << 8) | chunk->code[chunk->count - 1]);
    if (index >= chunk->constant_count || !lz_value_is_closure(&chunk->constants[index])) {
        return false;
    }
    *out = chunk->constants[index];
    lz_value_retain(out);
    lz_chunk_truncate(&parser->vm->heap, chunk, chunk->count - 3, chunk->constant_count);
    return true;
}

static LzString *qualified_name(Parser *parser, const LzString *cls, const LzString *member) {
    char text[200];
    int n = snprintf(text, sizeof text, "%s.%s", cls->data, member->data);
    if (n < 0) {
        return NULL;
    }
    return lz_vm_intern(parser->vm, text, (size_t)n < sizeof text ? (size_t)n : sizeof text - 1);
}

/*
 * `[pub] [override] operator + (self, other) ...`. The member is a method named by the symbol
 * ("+", "-", "*", "/", "%", "==", "<"; "-@" for the unary minus), which no identifier can clash with.
 */
static void operator_member(Parser *parser, LzClass *cls, LzVisibility vis, bool is_override) {
    advance(parser); /* operator */
    if (parser->in_trait) {
        error_at(parser, &parser->previous, "operators in traits are not supported yet");
        return;
    }
    LzToken op = parser->current;
    const char *symbol = NULL;
    char text[48];
    token_text(parser, &op, text, sizeof text);
    char message[160];
    switch (op.type) {
    case LZ_TOKEN_PLUS:
        symbol = "+";
        break;
    case LZ_TOKEN_MINUS:
        symbol = "-";
        break;
    case LZ_TOKEN_STAR:
        symbol = "*";
        break;
    case LZ_TOKEN_SLASH:
        symbol = "/";
        break;
    case LZ_TOKEN_PERCENT:
        symbol = "%";
        break;
    case LZ_TOKEN_EQUAL_EQUAL:
        symbol = "==";
        break;
    case LZ_TOKEN_LESS:
        symbol = "<";
        break;
    case LZ_TOKEN_GREATER:
    case LZ_TOKEN_LESS_EQUAL:
    case LZ_TOKEN_GREATER_EQUAL:
        (void)snprintf(message, sizeof message,
                       "operator '%s' cannot be defined; define '<' and '>', '<=' and '>=' follow from it", text);
        error_at(parser, &op, message);
        return;
    case LZ_TOKEN_BANG_EQUAL:
        error_at(parser, &op, "'!=' follows from '=='");
        return;
    case LZ_TOKEN_BANG:
        if (next_token_type(parser) == LZ_TOKEN_EQUAL_EQUAL) {
            error_at(parser, &op, "'!=' follows from '=='");
            return;
        }
        /* fall through */
    default:
        (void)snprintf(message, sizeof message, "operator '%s' cannot be defined", text);
        error_at(parser, &op, message);
        return;
    }
    advance(parser); /* the operator */
    if (!check(parser, LZ_TOKEN_LEFT_PAREN)) {
        error_at_current(parser, "expected '(' after the operator");
        return;
    }
    LzToken first_param = peek_token(parser, 1);
    if (!word_is(parser, &first_param, "self")) {
        error_at(parser, &op, "an operator takes 'self' first");
        return;
    }
    bool unary = op.type == LZ_TOKEN_MINUS && peek_token(parser, 2).type == LZ_TOKEN_RIGHT_PAREN;
    LzString *name = lz_vm_intern(parser->vm, unary ? "-@" : symbol, unary ? 2 : strlen(symbol));
    if (name == NULL) {
        fail(parser, lz_vm_out_of_memory(parser->vm));
        return;
    }
    if (lz_class_find(cls, name) != NULL) {
        (void)snprintf(message, sizeof message, "operator %s is defined twice in %s", text, cls->name->data);
        error_at(parser, &op, message);
        return;
    }
    LzMember *member = NULL;
    if (!lz_class_add_member(cls, name, LZ_MEMBER_METHOD, vis, op.offset, &member)) {
        fail(parser, lz_vm_out_of_memory(parser->vm));
        return;
    }
    member->is_override = is_override ? 1 : 0;
    size_t index = cls->member_count - 1;
    LzString *full = qualified_name(parser, cls->name, name);
    parser->member_pending = true;
    parser->member_static = false;
    (void)compile_function(parser, full, SIZE_MAX, op.offset);
    parser->member_pending = false;
    LzValue closure = lz_value_nil();
    if (take_member_closure(parser, &closure)) {
        cls->members[index].value = closure;
        const LzFunction *function = lz_value_as_closure(&closure)->function;
        if (!unary && function->param_count != 2) {
            (void)snprintf(message, sizeof message, "operator %s takes 'self' and one other value", text);
            error_at(parser, &op, message);
        }
    }
    end_statement(parser);
}

static void class_member(Parser *parser, LzClass *cls) {
    LzToken first = parser->current;
    LzVisibility vis = LZ_VIS_PRIVATE;
    bool read_only = false;
    bool has_pub = false;

    if (is_unsupported_word(parser, &first)) {
        LzTokenType after = next_token_type(parser);
        if (after != LZ_TOKEN_COLON && after != LZ_TOKEN_EQUAL && after != LZ_TOKEN_NEWLINE &&
            after != LZ_TOKEN_RIGHT_BRACE) {
            char word[32];
            char message[80];
            token_text(parser, &first, word, sizeof word);
            (void)snprintf(message, sizeof message, "'%s' is not supported yet", word);
            error_at(parser, &first, message);
            return;
        }
    }
    if (word_is(parser, &first, "pub")) {
        LzTokenType after = next_token_type(parser);
        if (after != LZ_TOKEN_COLON && after != LZ_TOKEN_EQUAL && after != LZ_TOKEN_NEWLINE) {
            advance(parser);
            has_pub = true;
            vis = LZ_VIS_PUB;
            LzToken word = parser->current;
            if (word_is(parser, &word, "read")) {
                LzTokenType next = next_token_type(parser);
                if (next == LZ_TOKEN_IDENTIFIER) {
                    advance(parser);
                    vis = LZ_VIS_PUB_READ;
                    read_only = true;
                }
            }
        }
    }

    bool is_override = false;
    LzToken override_token = parser->current;
    if (word_is(parser, &override_token, "override")) {
        LzTokenType after = next_token_type(parser);
        if (after == LZ_TOKEN_FN || after == LZ_TOKEN_IDENTIFIER || after == LZ_TOKEN_CONST) {
            advance(parser);
            is_override = true;
        }
    }
    /*
     * `pub link name: T` marks a field whose value is a non-owning reference. `link` is a word
     * only when a name follows it, so a field named `link` keeps working.
     */
    bool is_link = false;
    LzToken link_token = parser->current;
    if (word_is(parser, &link_token, "link")) {
        LzTokenType next = next_token_type(parser);
        /*
         * A name after `link` makes it the modifier (`link x: T`, `link get x()`); `link fn` and
         * `link const` are not fields, and the member check below rejects them with one message.
         * Anything else (`link: int`, `link`) keeps `link` as the field's own name.
         */
        if (next == LZ_TOKEN_IDENTIFIER || next == LZ_TOKEN_FN || next == LZ_TOKEN_CONST) {
            advance(parser);
            is_link = true;
        }
    }
    if (word_is(parser, &parser->current, "operator")) {
        LzTokenType after = next_token_type(parser);
        if (after != LZ_TOKEN_COLON && after != LZ_TOKEN_EQUAL && after != LZ_TOKEN_NEWLINE &&
            after != LZ_TOKEN_RIGHT_BRACE && after != LZ_TOKEN_SEMICOLON) {
            if (parser->in_extend) {
                error_at_current(parser, "operators cannot be added by extend yet");
                return;
            }
            operator_member(parser, cls, vis, is_override);
            return;
        }
    }
    LzToken head = parser->current;
    LzMemberKind kind = LZ_MEMBER_FIELD;
    if (head.type == LZ_TOKEN_FN && next_token_type(parser) == LZ_TOKEN_IDENTIFIER) {
        kind = LZ_MEMBER_METHOD;
        advance(parser);
    } else if (head.type == LZ_TOKEN_CONST) {
        kind = LZ_MEMBER_CONST;
        advance(parser);
    } else if (word_is(parser, &head, "get") && next_token_type(parser) == LZ_TOKEN_IDENTIFIER &&
               peek_token(parser, 2).type == LZ_TOKEN_LEFT_PAREN) {
        kind = LZ_MEMBER_GETTER;
        advance(parser);
    } else if (head.type != LZ_TOKEN_IDENTIFIER) {
        error_at(parser, &head, "only fields, functions, properties and constants can appear in a class body");
        return;
    } else if (next_token_type(parser) != LZ_TOKEN_COLON && next_token_type(parser) != LZ_TOKEN_EQUAL &&
               next_token_type(parser) != LZ_TOKEN_NEWLINE && next_token_type(parser) != LZ_TOKEN_RIGHT_BRACE &&
               next_token_type(parser) != LZ_TOKEN_SEMICOLON) {
        error_at(parser, &head, "only fields, functions, properties and constants can appear in a class body");
        return;
    }
    if (read_only && kind != LZ_MEMBER_FIELD) {
        error_at(parser, &first, "'read' only applies to fields");
        return;
    }
    if (is_link && kind != LZ_MEMBER_FIELD) {
        error_at(parser, &link_token, "a link can only be a field");
        return;
    }
    if (is_override && (kind == LZ_MEMBER_FIELD || kind == LZ_MEMBER_CONST)) {
        error_at(parser, &override_token, "'override' only applies to functions and properties");
        return;
    }
    (void)has_pub;
    if (parser->in_extend) {
        if (is_override) {
            error_at(parser, &override_token, "extend cannot override a method; it can only add new ones");
            return;
        }
        if (kind == LZ_MEMBER_FIELD) {
            error_at(parser, &head, "an extension cannot add fields");
            return;
        }
        if (kind == LZ_MEMBER_CONST) {
            error_at(parser, &head, "an extension cannot add constants");
            return;
        }
    }
    if (parser->in_trait) {
        vis = LZ_VIS_PUB; /* what a trait offers is public, written `pub` or not */
        if (kind == LZ_MEMBER_FIELD) {
            error_at(parser, &head, "a trait cannot have fields");
            return;
        }
        if (kind == LZ_MEMBER_CONST) {
            error_at(parser, &head, "a trait cannot have constants yet");
            return;
        }
    }

    if (!check(parser, LZ_TOKEN_IDENTIFIER)) {
        error_at_current(parser, kind == LZ_MEMBER_FIELD ? "expected a field name" : "expected a name");
        return;
    }
    advance(parser);
    LzToken name_token = parser->previous;
    LzString *name = intern_token(parser, &name_token);
    if (name == NULL) {
        return;
    }
    if (lz_class_find(cls, name) != NULL) {
        char message[128];
        char text[48];
        token_text(parser, &name_token, text, sizeof text);
        (void)snprintf(message, sizeof message, "'%s' is declared twice in %s", text, cls->name->data);
        parser->panic = true;
        record_full(parser, name_token.offset, name_token.length, message, "declared again here", NULL);
        return;
    }

    LzMember *member = NULL;
    LzHeap *heap = &parser->vm->heap;
    (void)heap;
    if (kind == LZ_MEMBER_FIELD) {
        if (cls->field_count >= LZ_CLASS_FIELDS_MAX) {
            error_at(parser, &name_token, "a class can have at most 64 fields");
            return;
        }
        if (!lz_class_add_member(cls, name, kind, vis, name_token.offset, &member)) {
            fail(parser, lz_vm_out_of_memory(parser->vm));
            return;
        }
        size_t index = cls->member_count - 1;
        cls->members[index].is_link = is_link ? 1 : 0;
        if (match(parser, LZ_TOKEN_COLON)) {
            LzType declared;
            if (parse_type_union(parser, &declared)) {
                cls->members[index].type = declared;
            }
        }
        if (match(parser, LZ_TOKEN_EQUAL)) {
            skip_newlines(parser);
            LzString *thunk_name = qualified_name(parser, cls->name, name);
            LzValue thunk = lz_value_nil();
            if (thunk_name != NULL && compile_thunk(parser, thunk_name, name_token.offset, &thunk)) {
                cls->members[index].thunk = thunk;
            }
        }
        end_statement(parser);
        return;
    }
    if (kind == LZ_MEMBER_CONST) {
        if (!lz_class_add_member(cls, name, kind, vis, name_token.offset, &member)) {
            fail(parser, lz_vm_out_of_memory(parser->vm));
            return;
        }
        size_t index = cls->member_count - 1;
        consume(parser, LZ_TOKEN_EQUAL, "expected '=' after the constant name");
        skip_newlines(parser);
        LzString *thunk_name = qualified_name(parser, cls->name, name);
        LzValue thunk = lz_value_nil();
        if (!parser->panic && thunk_name != NULL && compile_thunk(parser, thunk_name, name_token.offset, &thunk)) {
            cls->members[index].thunk = thunk;
        }
        end_statement(parser);
        return;
    }

    /* A method, a static function or a property: a function. `self` first makes it a method. */
    bool has_self = false;
    if (check(parser, LZ_TOKEN_LEFT_PAREN)) {
        LzToken param = peek_token(parser, 1);
        has_self = word_is(parser, &param, "self");
    }
    if (kind == LZ_MEMBER_GETTER) {
        if (!has_self) {
            error_at(parser, &name_token, "a 'get' property takes only 'self'");
            return;
        }
    } else if (!has_self) {
        kind = LZ_MEMBER_STATIC;
    }
    if (parser->in_extend) {
        if (kind == LZ_MEMBER_STATIC) {
            error_at(parser, &name_token, "an extension cannot add static functions yet");
            return;
        }
        if (kind == LZ_MEMBER_METHOD && strcmp(name->data, "init") == 0) {
            error_at(parser, &name_token, "an extension cannot add init()");
            return;
        }
    }
    if (parser->in_trait) {
        if (kind == LZ_MEMBER_STATIC) {
            error_at(parser, &name_token, "a trait cannot have static functions");
            return;
        }
        if (kind == LZ_MEMBER_METHOD && strcmp(name->data, "init") == 0) {
            error_at(parser, &name_token, "a trait cannot have init()");
            return;
        }
    }
    if (!lz_class_add_member(cls, name, kind, vis, name_token.offset, &member)) {
        fail(parser, lz_vm_out_of_memory(parser->vm));
        return;
    }
    member->is_override = is_override ? 1 : 0;
    if (is_override && kind == LZ_MEMBER_METHOD && strcmp(name->data, "init") == 0) {
        error_at(parser, &override_token, "init() runs automatically for every class; it cannot be 'override'");
    }
    size_t index = cls->member_count - 1;
    LzString *full = qualified_name(parser, cls->name, name);
    parser->member_pending = true;
    parser->member_static = !has_self;
    parser->last_bodiless = false;
    (void)compile_function(parser, full, SIZE_MAX, name_token.offset);
    parser->member_pending = false;
    if (parser->last_bodiless) {
        cls->members[index].is_required = 1;
        parser->last_bodiless = false;
    }
    LzValue closure = lz_value_nil();
    if (take_member_closure(parser, &closure)) {
        cls->members[index].value = closure;
        const LzFunction *function = lz_value_as_closure(&closure)->function;
        if (kind == LZ_MEMBER_GETTER && function->param_count != 1) {
            error_at(parser, &name_token, "a 'get' property takes only 'self'");
        } else if (kind == LZ_MEMBER_METHOD && strcmp(name->data, "init") == 0 && function->param_count != 1) {
            error_at(parser, &name_token, "init() takes only 'self'");
        }
    }
    end_statement(parser);
}

/* After a mistake in the head of a class: skip to its `{` and past the matching `}`. */
static void skip_class_body(Parser *parser) {
    while (!check(parser, LZ_TOKEN_EOF) && !check(parser, LZ_TOKEN_LEFT_BRACE) && !check(parser, LZ_TOKEN_NEWLINE)) {
        advance(parser);
    }
    if (!check(parser, LZ_TOKEN_LEFT_BRACE)) {
        return;
    }
    int target = parser->bracket_depth;
    advance(parser);
    while (parser->fatal == LZ_OK && !check(parser, LZ_TOKEN_EOF) && parser->bracket_depth > target) {
        advance(parser);
    }
}

static void class_declaration(Parser *parser) {
    LzToken class_token = parser->current;
    bool is_trait = word_is(parser, &class_token, "trait");
    advance(parser); /* class or trait */
    if (parser->fs->function != NULL || parser->scope_depth != 0) {
        error_at(parser, &class_token, "classes can only be declared at the top level");
        skip_class_body(parser);
        return;
    }
    advance(parser); /* the name */
    LzToken name_token = parser->previous;
    LzString *name = intern_token(parser, &name_token);
    if (name == NULL) {
        return;
    }
    LzType ignored;
    char text[48];
    token_text(parser, &name_token, text, sizeof text);
    if (builtin_type(text, &ignored)) {
        error_with_name(parser, &name_token, "cannot redefine builtin type '", "'");
        return;
    }
    /* `class Dog : Animal, Named`: the names are looked up when the statement runs. */
    LzToken parents[8];
    size_t parent_count = 0;
    if (match(parser, LZ_TOKEN_COLON)) {
        do {
            skip_newlines(parser);
            if (!check(parser, LZ_TOKEN_IDENTIFIER)) {
                error_at_current(parser, "expected the name of a parent class or trait");
                skip_class_body(parser);
                return;
            }
            advance(parser);
            LzToken parent = parser->previous;
            char parent_text[48];
            token_text(parser, &parent, parent_text, sizeof parent_text);
            if (builtin_type(parent_text, &ignored)) {
                error_at(parser, &parent, "cannot inherit from a builtin type");
                skip_class_body(parser);
                return;
            }
            if (parent_count == 8) {
                error_at(parser, &parent, "too many parents");
                skip_class_body(parser);
                return;
            }
            parents[parent_count++] = parent;
        } while (match(parser, LZ_TOKEN_COMMA));
    }
    if (!check(parser, LZ_TOKEN_LEFT_BRACE)) {
        error_at_current(parser, "expected '{' before the class body");
        return;
    }
    LzType type = lz_type_of_kinds(LZ_KIND_FUNCTION);
    Symbol *symbol = add_symbol(parser, &name_token, name, SYMBOL_GLOBAL, true, lz_value_nil(), &type);
    if (symbol == NULL) {
        return;
    }
    if (parser->class_count >= 64) {
        error_at(parser, &name_token, "too many classes in one file");
        return;
    }
    parser->vm->has_classes = true;

    LzClass *cls = lz_class_new(parser->vm, name, name_token.offset);
    if (cls == NULL) {
        fail(parser, lz_vm_out_of_memory(parser->vm));
        return;
    }
    cls->is_trait = is_trait;
    parser->class_ids[parser->class_count] = cls->id;
    parser->classes[parser->class_count++] = name;
    LzString *saved_class = parser->class_name;
    uint64_t saved_class_id = parser->current_class_id;
    parser->class_name = name;
    parser->current_class_id = cls->id;
    size_t saved_parent_count = parser->class_parent_count;
    parser->class_parent_count = parent_count;
    bool saved_in_trait = parser->in_trait;
    parser->in_trait = is_trait;

    advance(parser); /* { */
    int body_depth = parser->bracket_depth - 1; /* the depth once the closing brace is read */
    skip_newlines(parser);
    while (!check(parser, LZ_TOKEN_RIGHT_BRACE) && !check(parser, LZ_TOKEN_EOF) && parser->fatal == LZ_OK &&
           !parser->panic) {
        class_member(parser, cls);
        while (match(parser, LZ_TOKEN_NEWLINE) || match(parser, LZ_TOKEN_SEMICOLON)) {
        }
    }
    parser->class_name = saved_class;
    parser->current_class_id = saved_class_id;
    parser->class_parent_count = saved_parent_count;
    parser->in_trait = saved_in_trait;
    if (!parser->panic) {
        consume(parser, LZ_TOKEN_RIGHT_BRACE, "expected '}' after the class body");
    }
    if (parser->panic || parser->fatal != LZ_OK) {
        /* One mistake in the body is one error: skip the rest of the body. */
        while (parser->fatal == LZ_OK && !check(parser, LZ_TOKEN_EOF) && parser->bracket_depth > body_depth) {
            advance(parser);
        }
        lz_object_release(&parser->vm->heap, &cls->base);
        return;
    }
    lz_class_seal(cls);
    uint16_t index = add_constant(parser, lz_value_object(&cls->base), true);
    lz_object_release(&parser->vm->heap, &cls->base);
    emit_op_at(parser, LZ_OP_CONSTANT, name_token.offset);
    emit_u16_at(parser, index, name_token.offset);
    emit_named(parser, LZ_OP_DEFINE_GLOBAL, name, name_token.offset);
    for (size_t i = 0; i < parent_count && parser->fatal == LZ_OK; i++) {
        LzToken parent = parents[i];
        (void)variable_at(parser, &parent);
        LzString *parent_name = intern_token(parser, &parent);
        if (parent_name == NULL) {
            return;
        }
        uint16_t parent_index = add_constant(parser, lz_value_object(&parent_name->base), true);
        emit_op_at(parser, LZ_OP_CLASS_LINK, parent.offset);
        emit_u16_at(parser, index, parent.offset);
        emit_u16_at(parser, parent_index, parent.offset);
        emit_byte_at(parser, i == 0 ? 0 : 1, parent.offset);
    }
    emit_op_at(parser, LZ_OP_CLASS_INIT, name_token.offset);
    emit_u16_at(parser, index, name_token.offset);
    end_statement(parser);
}

/*
 * `extend Target { members }`: adds methods and properties to a type that exists. The block is a
 * hidden class (see src/core/extend.c); the statement that registers it runs at its place in the
 * file, so the names it adds exist from there on.
 */
static void extend_declaration(Parser *parser) {
    LzToken extend_token = parser->current;
    advance(parser); /* extend */
    if (parser->fs->function != NULL || parser->scope_depth != 0) {
        error_at(parser, &extend_token, "extend can only be used at the top level");
        skip_class_body(parser);
        return;
    }
    advance(parser); /* the target */
    LzToken target = parser->previous;
    char text[48];
    token_text(parser, &target, text, sizeof text);
    uint16_t kinds = 0;
    bool is_class = false;
    char message[160];
    if (check(parser, LZ_TOKEN_LEFT_BRACKET)) {
        error_at(parser, &target, "extend works on a whole kind such as 'array', not on 'int[]'");
        skip_class_body(parser);
        return;
    }
    if (strcmp(text, "string") == 0) {
        kinds = LZ_KIND_STRING;
    } else if (strcmp(text, "int") == 0) {
        kinds = LZ_KIND_INT;
    } else if (strcmp(text, "float") == 0) {
        kinds = LZ_KIND_FLOAT;
    } else if (strcmp(text, "bool") == 0) {
        kinds = LZ_KIND_BOOL;
    } else if (strcmp(text, "array") == 0) {
        kinds = LZ_KIND_ARRAY;
    } else if (strcmp(text, "dict") == 0) {
        kinds = LZ_KIND_DICT;
    } else if (strcmp(text, "nil") == 0) {
        kinds = LZ_KIND_NIL;
    } else if (strcmp(text, "function") == 0) {
        kinds = LZ_KIND_FUNCTION;
    } else {
        LzType ignored;
        if (builtin_type(text, &ignored)) {
            (void)snprintf(message, sizeof message, "'%s' is not a type that can be extended", text);
            error_at(parser, &target, message);
            skip_class_body(parser);
            return;
        }
        is_class = true;
    }
    LzString *name = intern_token(parser, &target);
    if (name == NULL) {
        return;
    }
    if (!check(parser, LZ_TOKEN_LEFT_BRACE)) {
        error_at_current(parser, "expected '{' before the extend body");
        return;
    }
    parser->vm->has_classes = true;
    LzClass *cls = lz_class_new(parser->vm, name, target.offset);
    if (cls == NULL) {
        fail(parser, lz_vm_out_of_memory(parser->vm));
        return;
    }
    cls->is_ext = true;
    cls->ext_kinds = kinds;
    cls->ext_target = name;
    lz_object_retain(&name->base);

    LzString *saved_class = parser->class_name;
    uint64_t saved_class_id = parser->current_class_id;
    size_t saved_parent_count = parser->class_parent_count;
    bool saved_in_trait = parser->in_trait;
    bool saved_in_extend = parser->in_extend;
    parser->class_name = name;
    parser->current_class_id = cls->id;
    parser->class_parent_count = 0;
    parser->in_trait = false;
    parser->in_extend = true;
    uint16_t saved_extend_kinds = parser->extend_kinds;
    parser->extend_kinds = kinds;

    advance(parser); /* { */
    int body_depth = parser->bracket_depth - 1;
    skip_newlines(parser);
    while (!check(parser, LZ_TOKEN_RIGHT_BRACE) && !check(parser, LZ_TOKEN_EOF) && parser->fatal == LZ_OK &&
           !parser->panic) {
        class_member(parser, cls);
        while (match(parser, LZ_TOKEN_NEWLINE) || match(parser, LZ_TOKEN_SEMICOLON)) {
        }
    }
    parser->class_name = saved_class;
    parser->current_class_id = saved_class_id;
    parser->class_parent_count = saved_parent_count;
    parser->in_trait = saved_in_trait;
    parser->in_extend = saved_in_extend;
    parser->extend_kinds = saved_extend_kinds;
    if (!parser->panic) {
        consume(parser, LZ_TOKEN_RIGHT_BRACE, "expected '}' after the extend body");
    }
    if (parser->panic || parser->fatal != LZ_OK) {
        while (parser->fatal == LZ_OK && !check(parser, LZ_TOKEN_EOF) && parser->bracket_depth > body_depth) {
            advance(parser);
        }
        lz_object_release(&parser->vm->heap, &cls->base);
        return;
    }
    for (size_t i = 0; i < cls->member_count; i++) {
        if (cls->members[i].is_required) {
            error_at(parser, &extend_token, "this function needs a body (only a trait can leave it out)");
            lz_object_release(&parser->vm->heap, &cls->base);
            return;
        }
    }
    uint16_t index = add_constant(parser, lz_value_object(&cls->base), true);
    lz_object_release(&parser->vm->heap, &cls->base);
    uint16_t name_index = add_constant(parser, lz_value_object(&name->base), true);
    emit_op_at(parser, LZ_OP_EXTEND, target.offset);
    emit_u16_at(parser, index, target.offset);
    emit_u16_at(parser, name_index, target.offset);
    emit_byte_at(parser, is_class ? 1 : 0, target.offset);
    end_statement(parser);
}

/*
 * `a.b = v`, `a.b += v` and `a.b.c = v`: the expression just compiled ends in a GET_FIELD (or
 * GET_FIELD_KEEP for a compound assignment), which becomes the SET_FIELD of the assignment.
 */
static bool member_assignment(Parser *parser, const Expr *target, bool finish) {
    bool plain = check(parser, LZ_TOKEN_EQUAL);
    bool compound = is_compound(parser->current.type);
    LzChunk *chunk = parser->chunk;
    if ((!plain && !compound) || !target->is_member || parser->fatal != LZ_OK ||
        chunk->count != target->op_pos + 3) {
        return false;
    }
    uint16_t name_index = (uint16_t)((chunk->code[target->op_pos + 1] << 8) | chunk->code[target->op_pos + 2]);
    uint32_t offset = chunk->offsets[target->op_pos];
    LzToken op_token = parser->current;
    LzTokenType assign = op_token.type;
    advance(parser);
    skip_newlines(parser);
    if (compound) {
        Expr value = expression(parser);
        Expr current = any_expr();
        (void)compound_value(parser, &op_token, &current, &value);
        emit_op_at(parser, compound_opcode(compound_operator(assign)), offset);
    } else {
        lz_chunk_truncate(&parser->vm->heap, chunk, target->op_pos, chunk->constant_count);
        (void)expression(parser);
    }
    emit_op_at(parser, LZ_OP_SET_FIELD, offset);
    emit_u16_at(parser, name_index, offset);
    if (finish) {
        end_statement(parser);
    }
    return true;
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
    if (parser->guard.active && parser->guard.func == parser->fs) {
        error_at_previous(parser, "cannot leave a 'finally' block with 'return'");
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
    bool run_finally = false;
    for (const TryContext *ctx = parser->try_ctx; ctx != NULL && ctx->func == parser->fs;
         ctx = ctx->enclosing) {
        run_finally = run_finally || ctx->has_finally;
    }
    if (run_finally) {
        /* The value waits in a local of its own while the finally bodies run. */
        begin_scope(parser);
        LzString *hidden_name = lz_vm_intern(parser->vm, "(return)", 8);
        LzType any = lz_type_of_kinds(LZ_KINDS_ANY);
        Symbol *hidden = hidden_name == NULL
                             ? NULL
                             : add_symbol(parser, &parser->previous, hidden_name, SYMBOL_LOCAL, false,
                                          lz_value_nil(), &any);
        if (hidden != NULL) {
            uint16_t slot = hidden->slot;
            emit_try_exits(parser, false, offset);
            emit_op_at(parser, LZ_OP_GET_LOCAL, offset);
            emit_u16_at(parser, slot, offset);
        }
        emit_op_at(parser, LZ_OP_RETURN, offset);
        end_scope(parser);
    } else {
        emit_try_exits(parser, false, offset); /* nothing to run: only closes handlers */
        emit_op_at(parser, LZ_OP_RETURN, offset);
    }
    end_statement(parser);
    parser->terminated = true;
}

/*
 * Top-level `fn` declarations are visible to the whole file, so functions can call
 * each other whatever their order. Only the names are declared here; the function
 * itself exists once its declaration has run.
 */
/* Error kinds: `error Name : Parent` */

typedef struct TopName {
    uint32_t offset;
    uint32_t length;
} TopName;

/* Does the text of this name match one of the top-level `let`, `const` and `fn` names? */
static bool top_name_used(const Parser *parser, const TopName *names, size_t count, uint32_t offset,
                          uint32_t length) {
    for (size_t i = 0; i < count; i++) {
        if (names[i].length == length &&
            memcmp(parser->source->text + names[i].offset, parser->source->text + offset,
                   length) == 0) {
            return true;
        }
    }
    return false;
}

/*
 * Declares the kind of one declaration in the VM, and remembers what went wrong in decl->problem.
 * A kind with an unknown parent is still declared (under Error), so the mistake is told once and
 * later uses of the name do not add to it. A kind that is a duplicate or collides is not.
 */
static void declare_error_kind(Parser *parser, ErrorDecl *decl, bool collides) {
    const char *text = parser->source->text;
    LzVM *vm = parser->vm;
    uint8_t existing;
    decl->problem = ERROR_DECL_OK;
    if (lz_vm_error_kind_from_name(vm, text + decl->name_offset, decl->name_length, &existing)) {
        decl->problem = ERROR_DECL_DUPLICATE;
        return;
    }
    LzString *name = lz_vm_intern(vm, text + decl->name_offset, decl->name_length);
    if (name == NULL) {
        fail(parser, lz_vm_out_of_memory(vm));
        return;
    }
    if (collides || lz_vm_lookup(vm, name) != NULL) {
        decl->problem = ERROR_DECL_COLLISION;
        return;
    }
    uint8_t parent = LZ_ERROR_ERROR;
    if (decl->has_parent &&
        !lz_vm_error_kind_from_name(vm, text + decl->parent_offset, decl->parent_length, &parent)) {
        parent = LZ_ERROR_ERROR;
        decl->problem = ERROR_DECL_UNKNOWN_PARENT;
    }
    uint8_t kind;
    LzStatus status = lz_vm_error_declare(vm, name, parent, &kind);
    if (status == LZ_ERR_RUNTIME) {
        decl->problem = ERROR_DECL_LIMIT; /* vm->error says how many */
    } else if (status != LZ_OK) {
        fail(parser, status);
    }
}

static ErrorDecl *add_error_decl(Parser *parser, const ErrorDecl *decl) {
    if (parser->error_decl_count == parser->error_decl_capacity) {
        size_t capacity = parser->error_decl_capacity != 0 ? parser->error_decl_capacity * 2 : 8;
        ErrorDecl *grown = realloc(parser->error_decls, capacity * sizeof *grown);
        if (grown == NULL) {
            fail(parser, LZ_ERR_MEMORY);
            return NULL;
        }
        parser->error_decls = grown;
        parser->error_decl_capacity = capacity;
    }
    parser->error_decls[parser->error_decl_count] = *decl;
    return &parser->error_decls[parser->error_decl_count++];
}

/*
 * Hoisting. Every `error` declaration at the top level of the file is found and declared in the
 * VM before the first statement is compiled, in the order they are written: a kind can be used
 * (caught, called) above its declaration, and the kinds are known without running the script.
 * Only declarations at the start of a statement outside every bracket count, which is what the
 * parser accepts (a declaration anywhere else is reported when the parser meets it).
 */
static void prescan_errors(Parser *parser) {
    LzLexer lexer;
    lz_lexer_init(&lexer, parser->source);
    TopName *names = NULL;
    size_t name_count = 0;
    size_t name_capacity = 0;
    ErrorDecl *found = NULL;
    size_t found_count = 0;
    size_t found_capacity = 0;
    bool at_start = true;
    bool ok = true;

    for (;;) {
        LzToken token = lz_lexer_next(&lexer);
        if (token.type == LZ_TOKEN_EOF) {
            break;
        }
        bool starts = at_start;
        at_start = token.type == LZ_TOKEN_NEWLINE || token.type == LZ_TOKEN_SEMICOLON ||
                   (token.type == LZ_TOKEN_RIGHT_BRACE && lexer.nesting == 0);
        if (!starts || lexer.nesting != 0 || !ok) {
            continue;
        }
        LzLexer peek = lexer;
        LzToken name = lz_lexer_next(&peek);
        if (name.type != LZ_TOKEN_IDENTIFIER) {
            continue;
        }
        if (token.type == LZ_TOKEN_LET || token.type == LZ_TOKEN_CONST || token.type == LZ_TOKEN_FN) {
            if (name_count == name_capacity) {
                size_t capacity = name_capacity != 0 ? name_capacity * 2 : 16;
                TopName *grown = realloc(names, capacity * sizeof *grown);
                if (grown == NULL) {
                    ok = false;
                    continue;
                }
                names = grown;
                name_capacity = capacity;
            }
            names[name_count++] = (TopName){(uint32_t)name.offset, (uint32_t)name.length};
        } else if (word_is(parser, &token, "error")) {
            ErrorDecl decl;
            memset(&decl, 0, sizeof decl);
            decl.name_offset = (uint32_t)name.offset;
            decl.name_length = (uint32_t)name.length;
            LzToken colon = lz_lexer_next(&peek);
            if (colon.type == LZ_TOKEN_COLON) {
                LzToken parent = lz_lexer_next(&peek);
                if (parent.type == LZ_TOKEN_IDENTIFIER) {
                    decl.has_parent = true;
                    decl.parent_offset = (uint32_t)parent.offset;
                    decl.parent_length = (uint32_t)parent.length;
                }
            }
            if (found_count == found_capacity) {
                size_t capacity = found_capacity != 0 ? found_capacity * 2 : 8;
                ErrorDecl *grown = realloc(found, capacity * sizeof *grown);
                if (grown == NULL) {
                    ok = false;
                    continue;
                }
                found = grown;
                found_capacity = capacity;
            }
            found[found_count++] = decl;
        }
    }

    if (!ok) {
        fail(parser, LZ_ERR_MEMORY);
    }
    for (size_t i = 0; i < found_count && parser->fatal == LZ_OK; i++) {
        bool collides = top_name_used(parser, names, name_count, found[i].name_offset,
                                      found[i].name_length);
        if (!collides) {
            LzString *name = lz_vm_intern(parser->vm, parser->source->text + found[i].name_offset,
                                          found[i].name_length);
            collides = name != NULL && find_symbol(parser, name) != NULL;
        }
        declare_error_kind(parser, &found[i], collides);
        if (add_error_decl(parser, &found[i]) == NULL) {
            break;
        }
    }
    free(names);
    free(found);
}

static const ErrorDecl *find_error_decl(const Parser *parser, uint32_t name_offset) {
    for (size_t i = 0; i < parser->error_decl_count; i++) {
        if (parser->error_decls[i].name_offset == name_offset) {
            return &parser->error_decls[i];
        }
    }
    return NULL;
}

/* A word that is `error`, at the start of a statement, with a name after it: a declaration. */
static bool at_error_declaration(const Parser *parser) {
    return word_is(parser, &parser->current, "error") &&
           next_token_type(parser) == LZ_TOKEN_IDENTIFIER;
}

/*
 * The body of `error Name : Parent { ... }`, or the empty class of an error whose parent has one: a
 * class in the sense of `class`, bound to the kind when the statement runs (it is not hoisted: its
 * defaults and `init` are code). The kind itself is declared by the prescan, as without a body.
 */
static void error_class_body(Parser *parser, const LzToken *name_token, uint8_t kind, bool has_body,
                             bool parent_class, const LzToken *traits, size_t trait_count) {
    LzString *name = intern_token(parser, name_token);
    if (name == NULL) {
        return;
    }
    parser->vm->has_classes = true;
    LzClass *cls = lz_class_new(parser->vm, name, name_token->offset);
    if (cls == NULL) {
        fail(parser, lz_vm_out_of_memory(parser->vm));
        return;
    }
    LzString *saved_class = parser->class_name;
    uint64_t saved_class_id = parser->current_class_id;
    size_t saved_parent_count = parser->class_parent_count;
    bool saved_in_trait = parser->in_trait;
    bool saved_in_extend = parser->in_extend;
    parser->class_name = name;
    parser->current_class_id = cls->id;
    parser->class_parent_count = parent_class ? 1 : 0;
    parser->in_trait = false;
    parser->in_extend = false;
    uint16_t saved_self_kind = parser->error_self_kind;
    parser->error_self_kind = (uint16_t)(kind + 1);
    int body_depth = parser->bracket_depth;
    if (has_body) {
        advance(parser); /* { */
        body_depth = parser->bracket_depth - 1;
        skip_newlines(parser);
        while (!check(parser, LZ_TOKEN_RIGHT_BRACE) && !check(parser, LZ_TOKEN_EOF) && parser->fatal == LZ_OK &&
               !parser->panic) {
            class_member(parser, cls);
            while (match(parser, LZ_TOKEN_NEWLINE) || match(parser, LZ_TOKEN_SEMICOLON)) {
            }
        }
    }
    parser->class_name = saved_class;
    parser->current_class_id = saved_class_id;
    parser->class_parent_count = saved_parent_count;
    parser->in_trait = saved_in_trait;
    parser->in_extend = saved_in_extend;
    parser->error_self_kind = saved_self_kind;
    if (has_body && !parser->panic) {
        consume(parser, LZ_TOKEN_RIGHT_BRACE, "expected '}' after the error body");
    }
    if (parser->panic || parser->fatal != LZ_OK) {
        while (parser->fatal == LZ_OK && !check(parser, LZ_TOKEN_EOF) && parser->bracket_depth > body_depth) {
            advance(parser);
        }
        lz_object_release(&parser->vm->heap, &cls->base);
        return;
    }
    lz_class_seal(cls);
    uint16_t index = add_constant(parser, lz_value_object(&cls->base), true);
    lz_object_release(&parser->vm->heap, &cls->base);
    uint16_t name_index = add_constant(parser, lz_value_object(&name->base), true);
    for (size_t i = 0; i < trait_count && parser->fatal == LZ_OK; i++) {
        LzToken trait = traits[i];
        (void)variable_at(parser, &trait);
        LzString *trait_name = intern_token(parser, &trait);
        if (trait_name == NULL) {
            return;
        }
        uint16_t trait_index = add_constant(parser, lz_value_object(&trait_name->base), true);
        emit_op_at(parser, LZ_OP_CLASS_LINK, trait.offset);
        emit_u16_at(parser, index, trait.offset);
        emit_u16_at(parser, trait_index, trait.offset);
        emit_byte_at(parser, 1, trait.offset);
    }
    emit_op_at(parser, LZ_OP_ERROR_CLASS, name_token->offset);
    emit_u16_at(parser, index, name_token->offset);
    emit_u16_at(parser, name_index, name_token->offset);
    emit_byte_at(parser, 0, name_token->offset);
    parser->error_class_kinds[kind / 64] |= (uint64_t)1 << (kind % 64);
    lz_vm_error_widen_constructor(parser->vm, kind);
    end_statement(parser);
}

/*
 * error Name [: Parent]. The kind is already declared by the prescan; here the declaration is
 * checked for where it stands and its mistakes are told. `error` is a keyword here and nowhere
 * else: anywhere but the start of a statement it is a name like any other.
 */
static void error_declaration(Parser *parser) {
    LzToken keyword = parser->current;
    bool nested = parser->bracket_depth != 0 || parser->fs->enclosing != NULL;
    advance(parser); /* error */
    advance(parser); /* the name */
    LzToken name_token = parser->previous;
    LzToken parent_token;
    memset(&parent_token, 0, sizeof parent_token);
    bool has_parent = false;
    LzToken traits[8];
    size_t trait_count = 0;
    if (match(parser, LZ_TOKEN_COLON)) {
        if (!check(parser, LZ_TOKEN_IDENTIFIER)) {
            error_at_current(parser, "expected the name of a parent error type after ':'");
            return;
        }
        advance(parser);
        parent_token = parser->previous;
        has_parent = true;
        /* `error E : Parent, T1, T2`: the first is the error parent, the rest are traits. */
        while (match(parser, LZ_TOKEN_COMMA)) {
            skip_newlines(parser);
            if (!check(parser, LZ_TOKEN_IDENTIFIER)) {
                error_at_current(parser, "expected the name of a trait");
                if (check(parser, LZ_TOKEN_LEFT_BRACE)) {
                    skip_class_body(parser);
                }
                return;
            }
            advance(parser);
            if (trait_count == 8) {
                error_at(parser, &parser->previous, "too many parents");
                if (check(parser, LZ_TOKEN_LEFT_BRACE)) {
                    skip_class_body(parser);
                }
                return;
            }
            traits[trait_count++] = parser->previous;
        }
    }
    if (nested) {
        error_at(parser, &keyword, "an 'error' declaration is only allowed at the top level");
        if (check(parser, LZ_TOKEN_LEFT_BRACE)) {
            skip_class_body(parser);
        }
        return;
    }

    const ErrorDecl *decl = find_error_decl(parser, name_token.offset);
    ErrorDecl late;
    if (decl == NULL) {
        /* The prescan did not see it (an odd layout): declare it now. */
        memset(&late, 0, sizeof late);
        late.name_offset = (uint32_t)name_token.offset;
        late.name_length = (uint32_t)name_token.length;
        late.has_parent = has_parent;
        late.parent_offset = (uint32_t)parent_token.offset;
        late.parent_length = (uint32_t)parent_token.length;
        declare_error_kind(parser, &late, false);
        decl = &late;
    }

    switch (decl->problem) {
    case ERROR_DECL_OK:
        break;
    case ERROR_DECL_DUPLICATE:
        error_with_name(parser, &name_token, "duplicate error type '", "'");
        if (check(parser, LZ_TOKEN_LEFT_BRACE)) {
            skip_class_body(parser);
        }
        return;
    case ERROR_DECL_COLLISION:
        error_with_name(parser, &name_token, "'", "' is already used by a variable or function");
        if (check(parser, LZ_TOKEN_LEFT_BRACE)) {
            skip_class_body(parser);
        }
        return;
    case ERROR_DECL_UNKNOWN_PARENT: {
        {
            LzString *parent_name = intern_token(parser, &parent_token);
            bool is_class = false;
            for (size_t i = 0; parent_name != NULL && i < parser->class_count; i++) {
                is_class = is_class || parser->classes[i] == parent_name;
            }
            const LzValue *bound = parent_name != NULL ? lz_table_get(&parser->vm->globals, parent_name) : NULL;
            if (is_class || (bound != NULL && lz_value_is_class(bound))) {
                error_with_name(parser, &parent_token, "'", "' is not an error kind");
                if (check(parser, LZ_TOKEN_LEFT_BRACE)) {
                    skip_class_body(parser);
                }
                return;
            }
        }
        LzSuggester suggester;
        lz_suggester_init(&suggester, parser->source->text + parent_token.offset,
                          parent_token.length);
        size_t count = lz_vm_error_kind_count(parser->vm);
        for (size_t i = 0; i < count; i++) {
            const char *kind_name = lz_vm_error_kind_name(parser->vm, (unsigned)i);
            /* The kind being declared is not a parent it could have meant. */
            if (strlen(kind_name) == name_token.length &&
                memcmp(kind_name, parser->source->text + name_token.offset, name_token.length) == 0) {
                continue;
            }
            lz_suggester_offer(&suggester, kind_name);
        }
        error_with_name_suggest(parser, &parent_token, "unknown error type '", "'",
                                "not a known error type", &suggester.found);
        if (check(parser, LZ_TOKEN_LEFT_BRACE)) {
            skip_class_body(parser);
        }
        return;
    }
    case ERROR_DECL_LIMIT: {
        char message[96];
        (void)snprintf(message, sizeof message, "too many error kinds (limit is %d)",
                       LZ_ERROR_TYPES_MAX);
        error_at(parser, &name_token, message);
        if (check(parser, LZ_TOKEN_LEFT_BRACE)) {
            skip_class_body(parser);
        }
        return;
    }
    }
    uint8_t kind = 0;
    uint8_t parent_kind = LZ_ERROR_ERROR;
    bool known = lz_vm_error_kind_from_name(parser->vm, parser->source->text + name_token.offset,
                                            name_token.length, &kind);
    if (known) {
        (void)lz_vm_error_kind_parent(parser->vm, kind, &parent_kind);
    }
    bool has_body = check(parser, LZ_TOKEN_LEFT_BRACE);
    bool parent_class = ((parser->error_class_kinds[parent_kind / 64] >> (parent_kind % 64)) & 1u) != 0 ||
                        lz_vm_error_class(parser->vm, parent_kind) != NULL;
    if (known && (has_body || parent_class || trait_count > 0)) {
        error_class_body(parser, &name_token, kind, has_body, parent_class, traits, trait_count);
        return;
    }
    end_statement(parser);
}

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
        case LZ_TOKEN_SWITCH:
        case LZ_TOKEN_TRY:
        case LZ_TOKEN_THROW:
        case LZ_TOKEN_WHILE:
        case LZ_TOKEN_FOR:
        case LZ_TOKEN_FN:
        case LZ_TOKEN_RETURN:
        case LZ_TOKEN_BREAK:
        case LZ_TOKEN_CONTINUE:
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
    case LZ_TOKEN_BREAK:
        advance(parser);
        loop_jump_statement(parser, true);
        break;
    case LZ_TOKEN_CONTINUE:
        advance(parser);
        loop_jump_statement(parser, false);
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
    case LZ_TOKEN_SWITCH:
        advance(parser);
        switch_statement(parser);
        break;
    case LZ_TOKEN_TRY:
        advance(parser);
        try_statement(parser);
        break;
    case LZ_TOKEN_THROW:
        advance(parser);
        throw_statement(parser);
        break;
    case LZ_TOKEN_WHILE:
        advance(parser);
        while_statement(parser);
        break;
    case LZ_TOKEN_FOR:
        advance(parser);
        for_statement(parser);
        break;
    case LZ_TOKEN_LEFT_BRACE:
        block(parser);
        break;
    case LZ_TOKEN_RIGHT_BRACE:
        error_at_current(parser, "unexpected '}'");
        break;
    case LZ_TOKEN_IDENTIFIER:
        if ((word_is(parser, &parser->current, "class") || word_is(parser, &parser->current, "trait")) &&
            next_token_type(parser) == LZ_TOKEN_IDENTIFIER) {
            class_declaration(parser);
        } else if (word_is(parser, &parser->current, "pub") && next_token_type(parser) == LZ_TOKEN_IDENTIFIER &&
                   word_is(parser, &(LzToken){.type = LZ_TOKEN_IDENTIFIER, .offset = peek_token(parser, 1).offset,
                                              .length = peek_token(parser, 1).length}, "class")) {
            error_at_current(parser, "'pub' on a class needs modules, which are not available yet");
            advance(parser); /* pub */
            advance(parser); /* class */
            skip_class_body(parser);
        } else if (word_is(parser, &parser->current, "extend") &&
                   (next_token_type(parser) == LZ_TOKEN_IDENTIFIER || next_token_type(parser) == LZ_TOKEN_NIL) &&
                   peek_token(parser, 2).type != LZ_TOKEN_EQUAL && peek_token(parser, 2).type != LZ_TOKEN_DOT &&
                   peek_token(parser, 2).type != LZ_TOKEN_LEFT_PAREN) {
            extend_declaration(parser);
        } else if (at_error_declaration(parser)) {
            error_declaration(parser);
        } else if (is_type_keyword(parser) && next_token_type(parser) == LZ_TOKEN_IDENTIFIER) {
            advance(parser);
            type_declaration(parser);
        } else if (next_token_type(parser) == LZ_TOKEN_EQUAL ||
                   is_compound(next_token_type(parser))) {
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


/* Does the file declare a class? Then every method call must look at its receiver's class. */
static void prescan_classes(Parser *parser) {
    LzLexer lexer;
    lz_lexer_init(&lexer, parser->source);
    bool at_start = true;
    for (;;) {
        LzToken token = lz_lexer_next(&lexer);
        if (token.type == LZ_TOKEN_EOF) {
            return;
        }
        bool starts = at_start;
        at_start = token.type == LZ_TOKEN_NEWLINE || token.type == LZ_TOKEN_SEMICOLON ||
                   (token.type == LZ_TOKEN_RIGHT_BRACE && lexer.nesting == 0);
        if (starts && lexer.nesting == 0 && word_is(parser, &token, "extend")) {
            LzLexer peek = lexer;
            LzToken name = lz_lexer_next(&peek);
            LzToken after = lz_lexer_next(&peek);
            if ((name.type == LZ_TOKEN_IDENTIFIER || name.type == LZ_TOKEN_NIL) && (after.type == LZ_TOKEN_LEFT_BRACE || after.type == LZ_TOKEN_LEFT_BRACKET)) {
                parser->vm->has_classes = true;
                return;
            }
        }
        if (starts && lexer.nesting == 0 && word_is(parser, &token, "class")) {
            LzLexer peek = lexer;
            LzToken name = lz_lexer_next(&peek);
            if (name.type == LZ_TOKEN_IDENTIFIER) {
                parser->vm->has_classes = true;
                return;
            }
        }
    }
}

static LzStatus compile_into(Parser *parser) {
    FuncState script;
    memset(&script, 0, sizeof script);
    script.self_symbol = SIZE_MAX;
    parser->fs = &script;
    prescan_functions(parser);
    prescan_errors(parser);
    prescan_classes(parser);
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
    parser->no_in_depth = -1;
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
    free(parser->error_decls);
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
