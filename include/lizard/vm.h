#ifndef LIZARD_VM_H
#define LIZARD_VM_H

#include "lizard/chunk.h"
#include "lizard/common.h"
#include "lizard/function.h"
#include "lizard/memory.h"
#include "lizard/skink.h"
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

struct LzVM {
    LzHeap heap;
    LzWriter out;
    LzWriter err;
    LzTable strings;
    LzTable globals;
    LzTable builtins;
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
    LzSkink skink; /* cycle collector, see skink.h; last so the JIT's field offsets stay put */
};

#define LZ_FUEL_UNLIMITED UINT64_MAX

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

/* Globals shadow builtins. name must be interned. NULL when undefined. */
const LzValue *lz_vm_lookup(const LzVM *vm, const LzString *name);

/*
 * Status for a failed allocation: LZ_ERR_HEAP_LIMIT when the configured limit
 * refused it, LZ_ERR_MEMORY otherwise. Natives call this when lz_heap_alloc
 * or lz_string_new returns NULL.
 */
LzStatus lz_vm_out_of_memory(LzVM *vm);

/* Records a message in vm->error and returns status so callers can `return` it directly. */
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
