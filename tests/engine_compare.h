#ifndef LIZARD_TEST_ENGINE_COMPARE_H
#define LIZARD_TEST_ENGINE_COMPARE_H

/*
 * Differential testing of execution engines: run the same program on the
 * reference interpreter and on the Gecko Engine, in two fresh VMs, and demand
 * that everything observable is identical.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lizard/builtin.h"
#include "lizard/compiler.h"
#include "lizard/engine.h"
#include "test_util.h"

typedef struct Outcome {
    LzStatus compile_status;
    LzStatus run_status;
    LzBuffer out;
    LzBuffer err;
    char error[256];
    char diagnostic[256]; /* first compile error, if any */
    uint32_t error_offset;
    uint64_t instructions;
    uint64_t fuel;
    size_t stack_count;
    size_t total_allocations;
    size_t peak_bytes;
    size_t live_objects; /* after the roots are released: anything left leaked */
    size_t live_bytes;
    LzEngineStats stats;
} Outcome;

static inline void outcome_begin(LzVM *vm, Outcome *outcome) {
    memset(outcome, 0, sizeof *outcome);
    lz_vm_init(vm, lz_writer_from_buffer(&outcome->out), lz_writer_from_buffer(&outcome->err));
    LZ_CHECK(lz_builtin_install(vm) == LZ_OK);
}

/* Records what the run left behind, then releases everything and checks for leaks. */
static inline void outcome_end(LzVM *vm, LzChunk *chunk, Outcome *outcome) {
    memcpy(outcome->error, vm->error, sizeof outcome->error);
    outcome->error_offset = vm->error_offset;
    outcome->instructions = vm->instructions;
    outcome->fuel = vm->fuel;
    outcome->stack_count = vm->stack_count;
    outcome->total_allocations = vm->heap.total_allocations;
    outcome->peak_bytes = vm->heap.peak_bytes;
    lz_chunk_free(&vm->heap, chunk);
    lz_vm_release_roots(vm);
    outcome->live_objects = vm->heap.live_objects;
    outcome->live_bytes = vm->heap.live_bytes;
    lz_vm_free(vm);
}

static inline void outcome_free(Outcome *outcome) {
    lz_buffer_free(&outcome->out);
    lz_buffer_free(&outcome->err);
}

/* Compiles text and runs it. A fuel or heap_limit of 0 means unlimited. */
static inline void run_source(LzEngine engine, const char *text, uint64_t fuel, size_t heap_limit,
                              Outcome *outcome) {
    LzVM vm;
    outcome_begin(&vm, outcome);

    LzSource source;
    lz_source_from_string(&source, "t.lz", text);
    LzChunk chunk;
    lz_chunk_init(&chunk);
    LzDiagnostics diagnostics;
    outcome->compile_status = lz_compile(&vm, &source, &chunk, &diagnostics);
    if (outcome->compile_status != LZ_OK && diagnostics.count > 0) {
        memcpy(outcome->diagnostic, diagnostics.items[0].message, sizeof outcome->diagnostic);
    }

    if (fuel != 0) {
        lz_vm_set_fuel(&vm, fuel);
    }
    if (heap_limit != 0) {
        lz_vm_set_heap_limit(&vm, heap_limit);
    }
    if (outcome->compile_status == LZ_OK) {
        outcome->run_status = lz_engine_run(engine, &vm, &chunk, &outcome->stats);
    }
    outcome_end(&vm, &chunk, outcome);
}

static inline const char *text_or_empty(const LzBuffer *buffer) {
    return buffer->data != NULL ? buffer->data : "";
}

static inline bool outcomes_equal(const Outcome *a, const Outcome *b) {
    return a->compile_status == b->compile_status && a->run_status == b->run_status &&
           strcmp(text_or_empty(&a->out), text_or_empty(&b->out)) == 0 &&
           strcmp(text_or_empty(&a->err), text_or_empty(&b->err)) == 0 &&
           strcmp(a->error, b->error) == 0 && a->error_offset == b->error_offset &&
           a->instructions == b->instructions && a->fuel == b->fuel &&
           a->stack_count == b->stack_count && a->total_allocations == b->total_allocations &&
           a->peak_bytes == b->peak_bytes && a->live_objects == b->live_objects &&
           a->live_bytes == b->live_bytes;
}

static inline void report_mismatch(const char *label, const char *program, uint64_t fuel,
                                   const Outcome *ref, const Outcome *ge) {
    fprintf(stderr, "MISMATCH %s (fuel %llu)\n--- program ---\n%s\n---------------\n", label,
            (unsigned long long)fuel, program);
#define SHOW(name, format, a, b)                                                                   \
    do {                                                                                           \
        if ((a) != (b)) {                                                                          \
            fprintf(stderr, "  %s: ref " format ", ge " format "\n", name, a, b);                  \
        }                                                                                          \
    } while (0)
    SHOW("compile status", "%d", (int)ref->compile_status, (int)ge->compile_status);
    SHOW("run status", "%d", (int)ref->run_status, (int)ge->run_status);
    if (strcmp(text_or_empty(&ref->out), text_or_empty(&ge->out)) != 0) {
        fprintf(stderr, "  stdout:\n    ref: [%s]\n    ge : [%s]\n", text_or_empty(&ref->out),
                text_or_empty(&ge->out));
    }
    if (strcmp(text_or_empty(&ref->err), text_or_empty(&ge->err)) != 0) {
        fprintf(stderr, "  stderr:\n    ref: [%s]\n    ge : [%s]\n", text_or_empty(&ref->err),
                text_or_empty(&ge->err));
    }
    if (strcmp(ref->error, ge->error) != 0) {
        fprintf(stderr, "  error message:\n    ref: [%s]\n    ge : [%s]\n", ref->error, ge->error);
    }
    SHOW("error offset", "%u", (unsigned)ref->error_offset, (unsigned)ge->error_offset);
    SHOW("instructions", "%llu", (unsigned long long)ref->instructions, (unsigned long long)ge->instructions);
    SHOW("fuel left", "%llu", (unsigned long long)ref->fuel, (unsigned long long)ge->fuel);
    SHOW("stack count", "%zu", ref->stack_count, ge->stack_count);
    SHOW("heap allocations", "%zu", ref->total_allocations, ge->total_allocations);
    SHOW("heap peak bytes", "%zu", ref->peak_bytes, ge->peak_bytes);
    SHOW("leaked objects", "%zu", ref->live_objects, ge->live_objects);
    SHOW("leaked bytes", "%zu", ref->live_bytes, ge->live_bytes);
#undef SHOW
}

/* True when both engines agree on everything observable and neither leaked. */
static inline bool outcomes_match(const char *label, const char *program, uint64_t fuel,
                                  const Outcome *ref, const Outcome *ge) {
    bool leaked = ref->live_objects != 0 || ref->live_bytes != 0 || ge->live_objects != 0 ||
                  ge->live_bytes != 0;
    if (outcomes_equal(ref, ge) && !leaked) {
        return true;
    }
    report_mismatch(label, program, fuel, ref, ge);
    if (leaked) {
        fprintf(stderr, "  leak: ref %zu objects, ge %zu objects\n", ref->live_objects, ge->live_objects);
    }
    return false;
}

/*
 * Runs a program on both engines and compares. With must_compile the program is
 * also required to compile and to be translated by GE (so a silent fallback to
 * the reference interpreter cannot make a test pass vacuously).
 */
static inline bool compare_source(const char *label, const char *text, uint64_t fuel, size_t heap_limit,
                                  bool must_compile) {
    Outcome ref;
    Outcome ge;
    run_source(LZ_ENGINE_REFERENCE, text, fuel, heap_limit, &ref);
    run_source(LZ_ENGINE_GE, text, fuel, heap_limit, &ge);
    bool same = outcomes_match(label, text, fuel, &ref, &ge);
    if (must_compile && ref.compile_status != LZ_OK) {
        fprintf(stderr, "DOES NOT COMPILE %s: %s\n--- program ---\n%s\n", label, ref.diagnostic, text);
        same = false;
    }
    if (ge.compile_status == LZ_OK && !ge.stats.translated) {
        fprintf(stderr, "NOT TRANSLATED %s\n--- program ---\n%s\n", label, text);
        same = false;
    }
    outcome_free(&ref);
    outcome_free(&ge);
    return same;
}


/* ---- bytecode built by hand or at random: the same chunk is built once per VM ---- */

typedef void (*ChunkBuilder)(LzVM *vm, LzChunk *chunk, void *context);

/* The source offset of every byte is its own index, so an error offset identifies the instruction. */
static inline void bc_byte(LzChunk *chunk, uint8_t byte) {
    (void)lz_chunk_write(chunk, byte, (uint32_t)chunk->count);
}

static inline void bc_op(LzChunk *chunk, LzOpCode op) {
    bc_byte(chunk, (uint8_t)op);
}

static inline void bc_u8(LzChunk *chunk, LzOpCode op, uint8_t operand) {
    bc_byte(chunk, (uint8_t)op);
    bc_byte(chunk, operand);
}

static inline void bc_u16(LzChunk *chunk, LzOpCode op, uint16_t operand) {
    bc_byte(chunk, (uint8_t)op);
    bc_byte(chunk, (uint8_t)(operand >> 8));
    bc_byte(chunk, (uint8_t)(operand & 0xFFu));
}

static inline uint16_t bc_constant(LzChunk *chunk, LzValue value) {
    uint16_t index = 0;
    (void)lz_chunk_add_constant(chunk, value, &index);
    return index;
}

static inline uint16_t bc_name(LzVM *vm, LzChunk *chunk, const char *name) {
    LzString *interned = lz_vm_intern(vm, name, strlen(name));
    return bc_constant(chunk, lz_value_object(&interned->base));
}

/* Globals every hand-built or random chunk can read: x = 5, y = 2.5, z = -3. */
static inline void define_test_globals(LzVM *vm) {
    static const char *const names[] = {"x", "y", "z"};
    LzValue values[3] = {lz_value_int(5), lz_value_float(2.5), lz_value_int(-3)};
    for (size_t i = 0; i < 3; i++) {
        LzString *name = lz_vm_intern(vm, names[i], 1);
        LZ_CHECK(name != NULL && lz_vm_define_global(vm, name, values[i]));
    }
}

static inline void run_chunk(LzEngine engine, ChunkBuilder build, void *context, uint64_t fuel,
                             Outcome *outcome) {
    LzVM vm;
    outcome_begin(&vm, outcome);
    define_test_globals(&vm);
    LzChunk chunk;
    lz_chunk_init(&chunk);
    build(&vm, &chunk, context);
    if (fuel != 0) {
        lz_vm_set_fuel(&vm, fuel);
    }
    outcome->run_status = lz_engine_run(engine, &vm, &chunk, &outcome->stats);
    outcome_end(&vm, &chunk, outcome);
}

/* Both engines on the same chunk. translated is what GE is expected to report (-1: don't care). */
static inline bool compare_chunk(const char *label, ChunkBuilder build, void *context, uint64_t fuel,
                                 int translated) {
    Outcome ref;
    Outcome ge;
    run_chunk(LZ_ENGINE_REFERENCE, build, context, fuel, &ref);
    run_chunk(LZ_ENGINE_GE, build, context, fuel, &ge);
    bool same = outcomes_match(label, "(bytecode)", fuel, &ref, &ge);
    if (translated >= 0 && ge.stats.translated != (translated != 0)) {
        fprintf(stderr, "TRANSLATION %s: expected %s, GE %s\n", label, translated ? "translated" : "not translated",
                ge.stats.translated ? "translated" : "fell back");
        same = false;
    }
    outcome_free(&ref);
    outcome_free(&ge);
    return same;
}

#endif
