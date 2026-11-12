#ifndef LIZARD_VM_H
#define LIZARD_VM_H

#include "lizard/chunk.h"
#include "lizard/error.h"
#include "lizard/common.h"
#include "lizard/function.h"
#include "lizard/memory.h"
#include "lizard/module.h"
#include "lizard/skink.h"
#include "lizard/suggest.h"
#include "lizard/table.h"
#include "lizard/writer.h"

#define LZ_STACK_MAX 16384

/* Deepest chain of nested calls, including the top-level script. */
#define LZ_FRAMES_MAX 2048

/* The mask of a call that passed every argument. */
#define LZ_ALL_SUPPLIED UINT64_MAX

/*
 * One running function, or the top-level script. The frame on top is the one executing, and the
 * others are paused at a call and resume where `ip` says.
 */
typedef struct LzCallFrame {
    LzClosure *closure;    /* NULL for the top-level script, which has no closure */
    const LzChunk *chunk;
    size_t ip;             /* resume point, set while a callee runs. The engine decides the unit */
    size_t base;           /* stack index of local slot 0, the first argument */
    uint64_t supplied;     /* bit i is set if the caller passed parameter i, otherwise its default runs */
    uint32_t call_offset;  /* byte offset of the call instruction, in the caller's chunk */
} LzCallFrame;

struct LzSource;

/* One line of the call stack at the moment a run failed. */
#define LZ_TRACE_MAX 16
typedef struct LzTraceEntry {
    char name[48];   /* the function, or "<script>" */
    uint32_t offset; /* source offset of the call, or of the failure for the innermost */
    const struct LzSource *source; /* the source that offset points into, or NULL for the main one (trace_source) */
} LzTraceEntry;

/*
 * A `try` that is open: where its catch code starts and what to cut the stack back to. Handlers
 * are a stack and the one on top is the innermost. See OP_TRY_BEGIN.
 */
typedef struct LzHandler {
    size_t frame;        /* index in vm->frames of the frame that holds the try */
    size_t target;       /* byte offset in that frame's chunk where its catch code starts */
    size_t stack_height; /* vm->stack_count when the try began */
} LzHandler;

#define LZ_HANDLERS_MAX 4096

/*
 * A kind of error declared with `error Name : Parent`. Its kind number is LZ_ERROR_KIND_COUNT +
 * index.
 */
typedef struct LzErrorType {
    LzString *name; /* interned, so the intern table owns it */
    uint8_t parent;
    struct LzClass *cls; /* the class of an error declared with a body, which is retained, or NULL */
    const struct LzModule *scope; /* the module that declared it, NULL for main. Only compared, never followed */
    bool dead;                    /* its module was thrown away, so no name finds it any more */
} LzErrorType;

/*
 * A visual fix for the run-time error in vm->error, see LzFix in report.h. It's an edit at a source
 * offset that the CLI draws as the corrected line. Optional. vm->error_help is what the CLI falls
 * back to when the edit can't be placed.
 */
typedef struct LzVMFix {
    bool present;
    uint32_t offset;
    uint32_t delete_length;
    char headline[128];
    char insert_text[160];
} LzVMFix;

struct LzVM {
    LzHeap heap;
    LzWriter out;
    LzWriter err;
    LzTable strings;
    LzTable globals;
    LzTable builtins;
    LzTable methods; /* dispatcher native for each method name, called as value.name(...) */
    LzType **array_types; /* interned item types of array types, see lz_vm_array_of */
    size_t array_type_count;
    size_t array_type_capacity;
    LzFnType **fn_types; /* interned signatures of function types, see lz_vm_fn_of */
    size_t fn_type_count;
    size_t fn_type_capacity;
    LzValue stack[LZ_STACK_MAX];
    size_t stack_count;
    LzCallFrame frames[LZ_FRAMES_MAX];
    size_t frame_count;         /* 0 while nothing runs */
    LzUpvalue *open_upvalues;   /* captured stack slots, highest first, and the list owns a reference */
    uint64_t fuel;
    uint64_t instructions;
    uint32_t error_offset; /* source offset of the failing instruction, or LZ_NO_OFFSET */
    const struct LzSource *error_source; /* the source error_offset points into, or NULL for the main one (trace_source) */
    LzTraceEntry trace[LZ_TRACE_MAX]; /* innermost first, and empty unless the failure was inside a function */
    size_t trace_count;
    size_t trace_total; /* frames in the whole stack, so a caller can say how many were left out */
    char error[256];
    uint8_t error_kind; /* the LzErrorKind of the failure in vm->error, a guess from its wording until it's caught */
    LzValue thrown;     /* the error value a `throw` is carrying, retained, or nil when there is none */
    LzValue last_thrown; /* the error that stopped the last run, kept for the report and retained */
    LzErrorType error_types[LZ_ERROR_TYPES_MAX - LZ_ERROR_KIND_COUNT];
    size_t error_type_count;
    LzHandler handlers[LZ_HANDLERS_MAX];
    size_t handler_count;
    const struct LzSource *trace_source; /* optional, and lets an error's trace say file:line:column */
    /*
     * "did you mean" names for the run-time error in vm->error, when it names a value that does not
     * exist, usually a dict key. Empty unless it does, and the CLI draws them like the ones the
     * compiler recorded.
     */
    LzSuggestions error_suggestions;
    uint32_t native_call_offset; /* source call site of the native that is running, for callback traces */
    unsigned native_depth;       /* callbacks nested inside natives right now, see lz_vm_call_value */
    const struct LzNative *current_native; /* the native lz_native_call is about to run, valid at its entry only */
    bool trace_fixed;            /* a failed callback already recorded the trace and its frames are gone */
    /* Classes, see class.h. */
    char error_help[384];        /* "help:" line for the run-time error in vm->error, or empty */
    char error_note[256];        /* "note:" line with context for the run-time error in vm->error, or empty */
    char error_label[160];       /* text written next to the carets of the run-time error, or empty */
    LzVMFix error_fix;           /* a visual fix for the run-time error, if any */
    LzStatus text_status;        /* a script's to_string or to_debug that failed while a value was being written */
    uint64_t next_class_id;      /* the last LzClass.id handed out */
    struct LzClass **class_registry; /* every live class sorted by id and held weakly, see lz_class_by_id */
    size_t class_registry_count;
    size_t class_registry_capacity;
    struct LzClass **exts;       /* the `extend` blocks that have run, in order, and they're retained */
    size_t ext_count;
    size_t ext_capacity;
    bool (*method_row_check)(const char *name, unsigned kinds); /* set by the builtin methods: is there a builtin row of this name for these kinds? */
    uint64_t ext_version;        /* bumped by every extend, and caches of method lookups compare it */
    bool has_classes;            /* set once some class is declared, so method calls must look at the receiver's class */
    LzObject *native_with;       /* the natives behind x.with(...) and x.copy() of an instance, retained and created lazily */
    LzObject *native_copy;
    /*
     * The link table, see link.h: a hash from each target to the cells that point at it. When a
     * target dies it sets object->linked and calls lz_link_target_freed so those cells are emptied
     * before the memory goes away. A cell that dies first removes itself from its row.
     */
    struct LzLinkRow **link_buckets;
    size_t link_bucket_count;
    size_t link_count;           /* cells in the table */
    size_t link_targets;         /* rows: targets that have at least one live cell */
    size_t link_cancelled;       /* cells emptied so far because their target was freed */
    uintptr_t c_stack_floor;     /* lowest C stack address a nested callback may reach, see call.c */
    uintptr_t c_stack_lo;        /* the thread stack the floor was measured on, as [lo, hi), because asking the system is slow */
    uintptr_t c_stack_hi;
    /* Modules, see module.h. They stay before skink so the JIT's field offsets don't move. */
    LzModule *modules;           /* every module, the main one first. The VM owns them */
    LzModule *main_module;       /* its globals are vm->globals */
    size_t module_count;
    LzModule *retired;           /* modules whose code failed while it ran, kept so borrowed pointers stay valid */
    LzModuleResolver resolver;   /* empty (resolve == NULL) means imports are not available */
    const LzModule **loading;    /* the modules whose code is being compiled or run, outermost first */
    size_t loading_count;
    size_t loading_capacity;
    struct LzModuleFailure *module_failure; /* why the last module failed to compile, or NULL */
    const struct LzModuleFailure *last_failure; /* the failure of the module the last prepare gave up on, or NULL */
    bool import_cycle;                          /* the module being compiled failed because of a cycle */
    /* Runs a compiled module once. The compiler sets it when a file imports another, for OP_IMPORT. */
    LzStatus (*module_run)(struct LzVM *vm, struct LzModule *module);
    size_t compiling;                       /* compiles in progress, so an import is nested when it is above 0 */
    LzSkink skink; /* cycle collector, see skink.h. It stays last so the JIT's field offsets don't move */
};

#define LZ_FUEL_UNLIMITED UINT64_MAX

/*
 * Deepest chain of callbacks run by natives and by classes, such as map inside map, a getter that
 * reads a getter, or a constructor whose init builds another object. Each one nests the C stack.
 * The count equals the frame limit, so a runaway recursion ends with the ordinary depth error
 * either way. The byte budget below protects the C stack itself, see lz_vm_call_value.
 */
#define LZ_NATIVE_DEPTH_MAX LZ_FRAMES_MAX

void lz_vm_init(LzVM *vm, LzWriter out, LzWriter err);

/* Caps live heap bytes for this VM. LZ_HEAP_UNLIMITED is the default and removes the cap. */
void lz_vm_set_heap_limit(LzVM *vm, size_t bytes);

/*
 * Sets the remaining instruction budget. Each executed instruction spends one unit, and the budget
 * persists across lz_vm_run calls, so hosts set it before every run. LZ_FUEL_UNLIMITED is the
 * default and turns metering off. vm->instructions counts executed instructions either way.
 */
void lz_vm_set_fuel(LzVM *vm, uint64_t fuel);

/*
 * Runs a chunk from its first byte until OP_RETURN. On failure it returns the status, vm->error
 * holds the message and vm->error_offset the source offset of the failing instruction. The value
 * stack is always empty afterwards.
 */
LzStatus lz_vm_run(LzVM *vm, const LzChunk *chunk);

/*
 * Like lz_vm_run, but on success it stores the value left on top of the stack in *result as a new
 * reference the caller owns. That's nil if the stack is empty.
 */
LzStatus lz_vm_eval(LzVM *vm, const LzChunk *chunk, LzValue *result);

/*
 * Extension points for alternative execution engines, see lizard/engine.h.
 *
 * lz_vm_exec_one runs the single instruction at byte offset *ip with the full reference semantics
 * and advances *ip past it. It does NOT charge fuel or count vm->instructions, so the caller does
 * both, exactly once per instruction. *halted is set when the instruction was OP_RETURN. An engine
 * that handles only the hot instructions itself can hand every other one to this function, so a
 * new opcode works on every engine the day it is added to run.c.
 *
 * lz_vm_eval_with is lz_vm_eval with the instruction loop supplied by the caller. The loop must
 * follow the contract of lz_vm_run: charge fuel before each instruction, count it, and report the
 * byte offset of the failing instruction through *failed_at.
 */
LzStatus lz_vm_exec_one(LzVM *vm, const LzChunk *chunk, size_t *ip, bool *halted);

/*
 * Runs the reference interpreter from the frame on top, at its saved ip in byte offsets, until the
 * frame stack is depth frames deep again. That means until that frame has returned. An engine that
 * can't run some function itself uses it to hand that one call to the reference interpreter. On
 * failure *failed_at is the byte offset of the failing instruction in the chunk of the frame on
 * top.
 */
LzStatus lz_vm_run_until(LzVM *vm, size_t depth, size_t *failed_at);

typedef LzStatus (*LzExecuteFn)(LzVM *vm, const LzChunk *chunk, size_t *failed_at, void *context);

LzStatus lz_vm_eval_with(LzVM *vm, const LzChunk *chunk, LzValue *result, LzExecuteFn run,
                         void *context);

/* Closes the open upvalues, drops every frame, and pops and releases every value on the stack. */
void lz_vm_reset_stack(LzVM *vm);

/*
 * These are the shared building blocks of every engine, so a call behaves the same everywhere.
 * Argument arrangement, meaning arity, defaults, the rest array and named arguments, lives here
 * and nowhere else.
 */

/*
 * Calls the value below the `argc` arguments on top of the stack. A native runs to completion and
 * its result replaces the callee and arguments, with *entered = false. For a user function the
 * arguments are arranged and a new frame is pushed, and the caller's frame remembers resume_ip
 * with *entered = true. The engine then runs the callee's code from its first instruction. On
 * failure no frame is pushed. call_offset is the byte offset of the call instruction, for stack
 * traces.
 */
LzStatus lz_vm_call(LzVM *vm, size_t argc, size_t resume_ip, uint32_t call_offset, bool *entered);

/*
 * Calls a function value from inside a native such as map or filter and runs it to the end. argv
 * are borrowed and *result is a new reference the caller owns. The callee may be a closure or
 * another native. On failure vm->error and vm->error_offset describe the failing instruction
 * inside the callback, and vm->trace holds the call stack at that moment. The callback's frames
 * are already unwound, so the native just returns the status.
 *
 * Callbacks run on the reference interpreter whatever engine started the program, and they nest
 * at most LZ_NATIVE_DEPTH_MAX deep. A native may call this many times. It must retain the items it
 * passes, because the callback can change the array they came from.
 */
LzStatus lz_vm_call_value(LzVM *vm, const LzValue *callee, size_t argc, const LzValue *argv,
                          LzValue *result);

/*
 * OP_CALL_EX: [callee][array of positional arguments][named_count named values]. names is the
 * constant holding the argument names, separated by NUL bytes. The array is spread onto the
 * stack, and then the call proceeds as lz_vm_call.
 */
LzStatus lz_vm_call_ex(LzVM *vm, const LzChunk *chunk, size_t named_count, uint16_t names_index,
                       size_t resume_ip, uint32_t call_offset, bool *entered);

/*
 * OP_RETURN inside a function. The value on top is the result. This closes the frame's upvalues,
 * drops the frame together with its callee, arguments and locals, and leaves the result where the
 * callee was. The caller's frame is on top afterwards.
 */
LzStatus lz_vm_return(LzVM *vm);

/* Records the call stack into vm->trace. Call it while the failing frames are still in place. */
void lz_vm_capture_trace(LzVM *vm);

/* Closes every open upvalue that lives at stack index >= level. */
void lz_vm_close_upvalues(LzVM *vm, size_t level);

/* OP_CLOSURE builds a closure from a function constant and pushes it. */
LzStatus lz_vm_make_closure(LzVM *vm, LzFunction *function);

/*
 * Drops every reference the VM itself holds, like interned names, globals and builtins. Afterwards
 * heap.live_objects counts objects that leaked.
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

/*
 * Methods live apart from globals and builtins, so `trim` the method and `trim` your variable
 * never meet.
 */
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
/*
 * A script's to_string or to_debug failed while a value was written. This takes that status,
 * once.
 */
bool lz_vm_take_text_status(LzVM *vm, LzStatus *status);
LzStatus lz_vm_set_error(LzVM *vm, LzStatus status, const char *format, ...) LZ_PRINTF_FORMAT(3, 4);

/*
 * Attach a "help:" line, which says what to do about it, or a "note:" line with context to the
 * error that lz_vm_set_error just recorded. Call them after lz_vm_set_error, which clears both.
 * Only write a help when there's a real fix that can be written as code.
 */
void lz_vm_set_help(LzVM *vm, const char *format, ...) LZ_PRINTF_FORMAT(2, 3);
void lz_vm_set_note(LzVM *vm, const char *format, ...) LZ_PRINTF_FORMAT(2, 3);
/*
 * The text next to the carets, like "`Person.relation` would own ...". Also call it after
 * lz_vm_set_error.
 */
void lz_vm_set_label(LzVM *vm, const char *format, ...) LZ_PRINTF_FORMAT(2, 3);
/*
 * A fix drawn under the error: delete `delete_length` bytes at `offset` and insert `insert_text`.
 * The help text stays as the fallback for a report that cannot draw it.
 */
void lz_vm_set_fix(LzVM *vm, const char *headline, uint32_t offset, size_t delete_length,
                   const char *insert_text);
/* Clears help, note, label and fix, which is what lz_vm_set_error does for a new failure. */
void lz_vm_clear_error_extras(LzVM *vm);

/*
 * Spends cost units of fuel. Natives that do heavy work call this so one instruction can't hide
 * unbounded effort. It fails with LZ_ERR_FUEL and leaves the budget at zero when it can't cover
 * the cost.
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
