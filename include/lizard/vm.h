#ifndef LIZARD_VM_H
#define LIZARD_VM_H

#include "lizard/chunk.h"
#include "lizard/error.h"
#include "lizard/common.h"
#include "lizard/function.h"
#include "lizard/memory.h"
#include "lizard/skink.h"
#include "lizard/suggest.h"
#include "lizard/table.h"
#include "lizard/writer.h"

#define LZ_STACK_MAX 16384

/* Deepest chain of nested calls, the top-level script included. */
#define LZ_FRAMES_MAX 2048

/* Every argument counts as supplied (the mask of a call that passed them all). */
#define LZ_ALL_SUPPLIED UINT64_MAX

/*
 * One running function (or the top-level script). The frame on top is the one
 * executing; the others are paused at a call and resume where `ip` says.
 */
typedef struct LzCallFrame {
    LzClosure *closure;    /* NULL for the top-level script, which has no closure */
    const LzChunk *chunk;  /* the code this frame runs */
    size_t ip;             /* resume point, set while a callee runs (the engine decides the unit) */
    size_t base;           /* stack index of local slot 0 (the first argument) */
    uint64_t supplied;     /* bit i: the caller passed parameter i (otherwise its default runs) */
    uint32_t call_offset;  /* byte offset of the call instruction, in the caller's chunk */
} LzCallFrame;

/* One line of the call stack at the moment a run failed. */
#define LZ_TRACE_MAX 16
typedef struct LzTraceEntry {
    char name[48];   /* the function, or "<script>" */
    uint32_t offset; /* source offset of the call (or of the failure, for the innermost) */
} LzTraceEntry;

/*
 * A `try` that is open: where its catch code starts and what to cut the stack back to. Handlers
 * are a stack; the one on top is the innermost. See OP_TRY_BEGIN.
 */
typedef struct LzHandler {
    size_t frame;        /* index in vm->frames of the frame that holds the try */
    size_t target;       /* byte offset in that frame's chunk where its catch code starts */
    size_t stack_height; /* vm->stack_count when the try began */
} LzHandler;

#define LZ_HANDLERS_MAX 4096

/* A kind of error declared with `error Name : Parent`; its kind number is LZ_ERROR_KIND_COUNT + index. */
typedef struct LzErrorType {
    LzString *name; /* interned: the intern table owns it */
    uint8_t parent;
    struct LzClass *cls; /* the class of an error declared with a body (retained), or NULL */
} LzErrorType;

struct LzSource;

struct LzVM {
    LzHeap heap;
    LzWriter out;
    LzWriter err;
    LzTable strings;
    LzTable globals;
    LzTable builtins;
    LzTable methods; /* name -> dispatcher native of the methods called as value.name(...) */
    LzType **array_types; /* interned item types of array types, see lz_vm_array_of */
    size_t array_type_count;
    size_t array_type_capacity;
    LzValue stack[LZ_STACK_MAX];
    size_t stack_count;
    LzCallFrame frames[LZ_FRAMES_MAX];
    size_t frame_count;         /* 0 while nothing runs */
    LzUpvalue *open_upvalues;   /* captured stack slots, highest slot first; the list owns a reference */
    uint64_t fuel;
    uint64_t instructions;
    uint32_t error_offset; /* source offset of the failing instruction, or LZ_NO_OFFSET */
    LzTraceEntry trace[LZ_TRACE_MAX]; /* innermost first; empty unless the failure was inside a function */
    size_t trace_count;
    size_t trace_total; /* frames in the whole stack, so a caller can say how many were left out */
    char error[256];
    uint8_t error_kind; /* the LzErrorKind of the failure in vm->error (a guess from its wording until caught) */
    LzValue thrown;     /* the error value a `throw` is carrying (retained), nil when there is none */
    LzValue last_thrown; /* the error that stopped the last run, kept for the report (retained) */
    LzErrorType error_types[LZ_ERROR_TYPES_MAX - LZ_ERROR_KIND_COUNT]; /* the declared kinds */
    size_t error_type_count;
    LzHandler handlers[LZ_HANDLERS_MAX];
    size_t handler_count;
    const struct LzSource *trace_source; /* optional: lets an error's trace say file:line:column */
    /*
     * "did you mean" names for the run-time error in vm->error, when it names a value
     * that does not exist (a dict key, usually). Empty unless it does; the CLI draws
     * them like the ones the compiler recorded.
     */
    LzSuggestions error_suggestions;
    uint32_t native_call_offset; /* source call site of the native that is running, for callback traces */
    unsigned native_depth;       /* callbacks nested inside natives right now, see lz_vm_call_value */
    const struct LzNative *current_native; /* the native lz_native_call is about to run; valid at its entry only */
    bool trace_fixed;            /* a failed callback already recorded the trace and its frames are gone */
    /* Classes (see class.h). */
    char error_help[192];        /* "help:" line for the run-time error in vm->error, or empty */
    LzStatus text_status;        /* a script to_string/to_debug that failed while a value was being written */
    uint64_t next_class_id;      /* the last LzClass.id handed out */
    struct LzClass **class_registry; /* every live class, sorted by id (weak): see lz_class_by_id */
    size_t class_registry_count;
    size_t class_registry_capacity;
    struct LzClass **exts;       /* the `extend` blocks that have run, in order (retained) */
    size_t ext_count;
    size_t ext_capacity;
    bool (*method_row_check)(const char *name, unsigned kinds); /* set by the builtin methods: is there a builtin row of this name for these kinds? */
    uint64_t ext_version;        /* bumped by every extend: caches of method lookups compare it */
    bool has_classes;            /* some class was declared: method calls must look at the receiver's class */
    LzObject *native_with;       /* the natives behind x.with(...) and x.copy() of an instance (retained, lazy) */
    LzObject *native_copy;
    /*
     * The link table (see link.h): a hash of target -> the cells that point at it. A target that
     * dies sets object->linked and asks lz_link_target_freed so those cells are emptied before
     * the memory goes away; a cell that dies first removes itself from its row.
     */
    struct LzLinkRow **link_buckets;
    size_t link_bucket_count;
    size_t link_count;           /* cells in the table */
    size_t link_targets;         /* rows: targets that have at least one live cell */
    size_t link_cancelled;       /* cells emptied so far because their target was freed */
    uintptr_t c_stack_floor;     /* lowest C stack address a nested callback may reach (see call.c) */
    uintptr_t c_stack_lo;        /* the thread stack the floor was measured on: [lo, hi); asking the system is slow */
    uintptr_t c_stack_hi;
    LzSkink skink; /* cycle collector, see skink.h; last so the JIT's field offsets stay put */
};

#define LZ_FUEL_UNLIMITED UINT64_MAX

/*
 * Deepest chain of callbacks run by natives and by classes (map inside map, a getter that reads a
 * getter, a constructor whose init builds another object ...): each one nests the C stack. The
 * count is the same as the frame limit, so a runaway recursion ends with the ordinary depth error
 * either way. The byte budget below protects the C stack itself (see lz_vm_call_value).
 */
#define LZ_NATIVE_DEPTH_MAX LZ_FRAMES_MAX

void lz_vm_init(LzVM *vm, LzWriter out, LzWriter err);

/* Caps live heap bytes for this VM; LZ_HEAP_UNLIMITED (the default) removes the cap. */
void lz_vm_set_heap_limit(LzVM *vm, size_t bytes);

/*
 * Sets the remaining instruction budget. Each executed instruction spends one
 * unit, and the budget persists across lz_vm_run calls, so hosts set it before
 * every run. LZ_FUEL_UNLIMITED (the default) turns metering off.
 * vm->instructions counts executed instructions either way.
 */
void lz_vm_set_fuel(LzVM *vm, uint64_t fuel);

/*
 * Executes a chunk from its first byte until OP_RETURN. On failure the status
 * is returned, vm->error holds the message and vm->error_offset the source
 * offset of the failing instruction. The value stack is always empty afterwards.
 */
LzStatus lz_vm_run(LzVM *vm, const LzChunk *chunk);

/*
 * Like lz_vm_run, but on success stores the value left on top of the stack
 * (nil if the stack is empty) in *result as a new reference the caller owns.
 */
LzStatus lz_vm_eval(LzVM *vm, const LzChunk *chunk, LzValue *result);

/*
 * Extension points for alternative execution engines (see lizard/engine.h).
 *
 * lz_vm_exec_one runs the single instruction at byte offset *ip with the full
 * reference semantics and advances *ip past it. It does NOT charge fuel or
 * count vm->instructions; the caller does both, exactly once per instruction.
 * *halted is set when the instruction was OP_RETURN. An engine that handles
 * only the hot instructions itself can hand every other one to this function,
 * so a new opcode works on every engine the day it is added to run.c.
 *
 * lz_vm_eval_with is lz_vm_eval with the instruction loop supplied by the
 * caller. The loop must follow the contract of lz_vm_run: charge fuel before
 * each instruction, count it, and report the byte offset of the failing
 * instruction through *failed_at.
 */
LzStatus lz_vm_exec_one(LzVM *vm, const LzChunk *chunk, size_t *ip, bool *halted);

/*
 * Runs the reference interpreter from the frame on top (at its saved ip, in byte
 * offsets) until the frame stack is depth frames deep again, that is, until that
 * frame has returned. An engine that cannot run some function itself uses it to hand
 * that one call to the reference interpreter. On failure *failed_at is the byte
 * offset of the failing instruction in the chunk of the frame on top.
 */
LzStatus lz_vm_run_until(LzVM *vm, size_t depth, size_t *failed_at);

typedef LzStatus (*LzExecuteFn)(LzVM *vm, const LzChunk *chunk, size_t *failed_at, void *context);

LzStatus lz_vm_eval_with(LzVM *vm, const LzChunk *chunk, LzValue *result, LzExecuteFn run,
                         void *context);

/* Closes the open upvalues and drops every frame; pops and releases every value on the stack. */
void lz_vm_reset_stack(LzVM *vm);

/*
 * Calls.
 *
 * These are the shared building blocks of every engine, so a call behaves the same
 * everywhere: argument arrangement (arity, defaults, the rest array, named
 * arguments) lives here and nowhere else.
 */

/*
 * Calls the value below the `argc` arguments on top of the stack. A native runs to
 * completion and its result replaces the callee and arguments (*entered = false).
 * A user function gets its arguments arranged and a new frame is pushed, and the
 * caller's frame remembers resume_ip (*entered = true); the engine then runs the
 * callee's code from its first instruction. On failure no frame is pushed.
 * call_offset is the byte offset of the call instruction, for stack traces.
 */
LzStatus lz_vm_call(LzVM *vm, size_t argc, size_t resume_ip, uint32_t call_offset, bool *entered);

/*
 * Calls a function value from inside a native (map, filter, ...) and runs it to the
 * end. argv are borrowed; *result is a new reference the caller owns. The callee may be
 * a closure or another native. On failure vm->error and vm->error_offset describe the
 * failing instruction inside the callback and vm->trace the call stack at that moment;
 * the callback's frames are already unwound, so the native just returns the status.
 *
 * Callbacks run on the reference interpreter whatever engine started the program, and
 * nest at most LZ_NATIVE_DEPTH_MAX deep. A native may call this many times; the items it
 * passes must be retained by the native, because the callback can change the array they
 * came from.
 */
LzStatus lz_vm_call_value(LzVM *vm, const LzValue *callee, size_t argc, const LzValue *argv,
                          LzValue *result);

/*
 * OP_CALL_EX: [callee][array of positional arguments][named_count named values].
 * names is the constant holding the argument names, separated by NUL bytes. The
 * array is spread onto the stack, then the call proceeds as lz_vm_call.
 */
LzStatus lz_vm_call_ex(LzVM *vm, const LzChunk *chunk, size_t named_count, uint16_t names_index,
                       size_t resume_ip, uint32_t call_offset, bool *entered);

/*
 * OP_RETURN inside a function: the value on top is the result. Closes the frame's
 * upvalues, drops the frame together with its callee, arguments and locals, and
 * leaves the result where the callee was. The caller's frame is on top afterwards.
 */
LzStatus lz_vm_return(LzVM *vm);

/* Records the call stack into vm->trace. Call while the failing frames are still in place. */
void lz_vm_capture_trace(LzVM *vm);

/* Closes every open upvalue that lives at stack index >= level. */
void lz_vm_close_upvalues(LzVM *vm, size_t level);

/* OP_CLOSURE: builds a closure of function (a constant) and pushes it. */
LzStatus lz_vm_make_closure(LzVM *vm, LzFunction *function);

/*
 * Drops every reference the VM itself holds (interned names, globals,
 * builtins). Afterwards heap.live_objects counts objects that leaked.
 */
void lz_vm_release_roots(LzVM *vm);

/* Flushes both writers, releases the roots and frees the heap. */
void lz_vm_free(LzVM *vm);

/*
 * Returns the unique string for these characters. The intern table owns the
 * reference, so the caller must not release the result. NULL when out of memory.
 */
LzString *lz_vm_intern(LzVM *vm, const char *chars, size_t length);

bool lz_vm_define_global(LzVM *vm, LzString *name, LzValue value);
bool lz_vm_define_builtin(LzVM *vm, LzString *name, LzValue value);

/* Methods live apart from globals and builtins, so `trim` the method and `trim` your variable never meet. */
bool lz_vm_define_method(LzVM *vm, LzString *name, LzValue value);

/* Globals shadow builtins. name must be interned. NULL when undefined. */
const LzValue *lz_vm_lookup(const LzVM *vm, const LzString *name);

/*
 * Status for a failed allocation: LZ_ERR_HEAP_LIMIT when the configured limit
 * refused it, LZ_ERR_MEMORY otherwise. Natives call this when lz_heap_alloc
 * or lz_string_new returns NULL.
 */
LzStatus lz_vm_out_of_memory(LzVM *vm);

/* Records a message in vm->error and returns status so callers can `return` it directly. */
/* A script to_string/to_debug failed while a value was written: takes that status (once). */
bool lz_vm_take_text_status(LzVM *vm, LzStatus *status);
LzStatus lz_vm_set_error(LzVM *vm, LzStatus status, const char *format, ...) LZ_PRINTF_FORMAT(3, 4);

/*
 * Spends cost units of fuel; natives that do heavy work call this so one
 * instruction cannot hide unbounded effort. Fails with LZ_ERR_FUEL and leaves
 * the budget at zero when it cannot cover the cost.
 */
static inline LzStatus lz_vm_charge_fuel(LzVM *vm, uint64_t cost) {
    if (vm->fuel == LZ_FUEL_UNLIMITED) {
        return LZ_OK;
    }
    if (cost > vm->fuel) {
        vm->fuel = 0;
        return lz_vm_set_error(vm, LZ_ERR_FUEL, "instruction limit exceeded");
    }
    vm->fuel -= cost;
    return LZ_OK;
}

#endif
