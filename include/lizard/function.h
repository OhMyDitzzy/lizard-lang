#ifndef LIZARD_FUNCTION_H
#define LIZARD_FUNCTION_H

#include "lizard/chunk.h"

/*
 * User-defined functions.
 *
 * Three heap objects cooperate:
 *
 *   LzFunction  the compiled code of one `fn`: its own chunk, its signature and
 *               which variables of the enclosing functions it captures. It is
 *               immutable after compilation and only ever appears in a chunk's
 *               constant table (OP_CLOSURE builds a callable from it).
 *   LzClosure   what a script can hold and call (kind `function`): a function
 *               plus the upvalues it captured. A function that captures nothing
 *               is created once, at compile time, and used as a plain constant.
 *   LzUpvalue   one captured variable. While the variable's frame is alive the
 *               upvalue points at the stack slot itself (open), so the frame and
 *               every closure see the same variable. When the frame ends the
 *               value moves into the upvalue (closed).
 *
 * Calling convention: the callee sits on the stack, the arguments above it. The
 * new frame's slot 0 is the first argument, so locals are addressed relative to
 * the frame base (OP_GET_LOCAL slot -> stack[base + slot]). Parameters come
 * first, then the rest array if there is one, then the function's own locals.
 */

#define LZ_MAX_PARAMS 64 /* positional parameters; one more slot holds a rest array */
#define LZ_MAX_UPVALUES 256

typedef struct LzUpvalueDesc {
    uint16_t index; /* is_local: slot in the enclosing frame; else: upvalue of the enclosing closure */
    bool is_local;
} LzUpvalueDesc;

typedef struct LzFunction {
    LzObject base;
    LzString *name; /* NULL for an anonymous function */
    LzChunk chunk;
    uint16_t param_count; /* positional parameters, not counting the rest parameter */
    uint16_t required;    /* the first `required` parameters have no default */
    bool has_rest;        /* `*rest` collects the surplus arguments into an array */
    uint16_t upvalue_count;
    LzUpvalueDesc *upvalues;
    LzString **param_names; /* param_count entries, plus the rest parameter's name when has_rest */
    uint32_t offset;        /* source offset of the definition, for diagnostics */
    /*
     * Opaque per-function data owned by an execution engine (the Gecko Engine keeps
     * its translated program here). Released through engine_cache_free.
     */
    void *engine_cache;
    void (*engine_cache_free)(void *cache);
} LzFunction;

/* A link of the cycle collector's watch lists (skink.h). next == NULL: on no list. */
typedef struct LzWatchLink {
    struct LzWatchLink *prev;
    struct LzWatchLink *next;
} LzWatchLink;

/* Needs no list head: the lists are circular, so a neighbour is all there is to patch. */
static inline void lz_watch_unlink(LzWatchLink *link) {
    link->prev->next = link->next;
    link->next->prev = link->prev;
    link->prev = NULL;
    link->next = NULL;
}

typedef struct LzUpvalue {
    LzObject base;
    LzValue *location; /* the stack slot while open, &closed afterwards */
    LzValue closed;
    struct LzUpvalue *next_open; /* the VM's list of open upvalues, highest slot first */
    LzWatchLink watch;           /* last: the engines address the fields above by offset */
} LzUpvalue;

typedef struct LzClosure {
    LzObject base;
    LzFunction *function;
    uint16_t upvalue_count;
    LzUpvalue *upvalues[]; /* upvalue_count entries; NULL only while being built */
} LzClosure;

/* The function holds its own references to name (may be NULL). NULL when out of memory. */
LzFunction *lz_function_new(LzHeap *heap, LzString *name);

/* Sets the parameter names (retained). param_count entries plus one when has_rest. */
bool lz_function_set_params(LzFunction *function, LzString *const *names, size_t param_count,
                            size_t required, bool has_rest);

bool lz_function_set_upvalues(LzFunction *function, const LzUpvalueDesc *descs, size_t count);

/* Retains function. The upvalue slots start out NULL. NULL when out of memory. */
LzClosure *lz_closure_new(LzHeap *heap, LzFunction *function);

/* A new open upvalue for the slot, with refcount 1 (the VM's list owns that reference). */
LzUpvalue *lz_upvalue_new(LzHeap *heap, LzValue *slot);

static inline bool lz_value_is_closure(const LzValue *value) {
    return value->type == LZ_VAL_OBJECT && value->as.object->type == LZ_OBJ_CLOSURE;
}

static inline LzClosure *lz_value_as_closure(const LzValue *value) {
    return (LzClosure *)value->as.object;
}

/* The name to show for a function: "add", or "<fn>" for an anonymous one. */
const char *lz_function_display_name(const LzFunction *function);

/* Total slots a call of this function arranges for its parameters. */
static inline size_t lz_function_slots(const LzFunction *function) {
    return (size_t)function->param_count + (function->has_rest ? 1u : 0u);
}

/*
 * Frees what the function owns outside its own block. With release_refs the
 * references it holds are released as well; the heap passes false when it frees
 * leaked objects wholesale, because the objects they point at are freed too.
 */
void lz_function_free_storage(LzHeap *heap, LzFunction *function, bool release_refs);

#endif
