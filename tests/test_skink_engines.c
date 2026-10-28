#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "lizard/builtin.h"
#include "lizard/compiler.h"
#include "lizard/engine.h"
#include "lizard/skink.h"
#include "lizard/vm.h"
#include "test_util.h"

/*
 * The engines must agree on when Skink frees what, down to the heap statistics: the
 * collector hangs off the closing of upvalues, which every engine does at the same
 * instructions, and its watch lists tick only on those.
 */

typedef struct Outcome {
    char output[256];
    LzStatus status;
    size_t allocations;
    size_t peak_bytes;
    size_t live_at_exit;
    LzSkinkStats stats;
} Outcome;

static size_t heap_limit = 0; /* applied after the builtins, as the CLI does */

static void run_with(const char *text, LzEngine engine, LzJitMode jit, Outcome *outcome) {
    memset(outcome, 0, sizeof *outcome);
    LzBuffer out = {0};
    LzBuffer err = {0};
    LzVM vm;
    lz_vm_init(&vm, lz_writer_from_buffer(&out), lz_writer_from_buffer(&err));
    LZ_CHECK(lz_builtin_install(&vm) == LZ_OK);
    if (heap_limit != 0) {
        lz_vm_set_heap_limit(&vm, heap_limit);
    }
    LzSource source;
    lz_source_from_string(&source, "t.lz", text);
    LzChunk chunk;
    lz_chunk_init(&chunk);
    LzDiagnostics diagnostics;
    LZ_CHECK(lz_compile(&vm, &source, &chunk, &diagnostics) == LZ_OK);
    lz_engine_set_jit(jit);
    outcome->status = lz_engine_run(engine, &vm, &chunk, NULL);
    if (out.data != NULL) {
        (void)snprintf(outcome->output, sizeof outcome->output, "%s", out.data);
    }
    outcome->stats = vm.skink.stats;
    lz_chunk_free(&vm.heap, &chunk);
    lz_vm_release_roots(&vm);
    outcome->allocations = vm.heap.total_allocations;
    outcome->peak_bytes = vm.heap.peak_bytes;
    outcome->live_at_exit = vm.heap.live_objects;
    outcome->stats = vm.skink.stats; /* after the sweep */
    lz_vm_free(&vm);
    lz_buffer_free(&out);
    lz_buffer_free(&err);
}

static bool same(const Outcome *a, const Outcome *b) {
    return a->status == b->status && strcmp(a->output, b->output) == 0 &&
           a->allocations == b->allocations && a->peak_bytes == b->peak_bytes &&
           a->live_at_exit == b->live_at_exit && memcmp(&a->stats, &b->stats, sizeof a->stats) == 0;
}

static void check_program(const char *name, const char *text, bool expect_clean) {
    Outcome reference;
    Outcome gecko;
    Outcome jitted;
    run_with(text, LZ_ENGINE_REFERENCE, LZ_JIT_OFF, &reference);
    run_with(text, LZ_ENGINE_GE, LZ_JIT_OFF, &gecko);
    run_with(text, LZ_ENGINE_GE, LZ_JIT_EAGER, &jitted);
    if (!same(&reference, &gecko) || !same(&reference, &jitted)) {
        fprintf(stderr, "%s: engines disagree (peak %zu/%zu/%zu, cycles %llu/%llu/%llu)\n", name,
                reference.peak_bytes, gecko.peak_bytes, jitted.peak_bytes,
                (unsigned long long)reference.stats.cycles, (unsigned long long)gecko.stats.cycles,
                (unsigned long long)jitted.stats.cycles);
    }
    LZ_CHECK(same(&reference, &gecko));
    LZ_CHECK(same(&reference, &jitted));
    LZ_CHECK(reference.stats.inconsistent == 0);
    LZ_CHECK(!expect_clean || reference.live_at_exit == 0);
}

#define TICK "fn tick() {\n  let k = 0\n  let g = fn() { return k }\n  return 0\n}\n"
#define MAKE_SELF "fn make() {\n  let f = nil\n  f = fn() { return f }\n  return f\n}\n"

int main(void) {
    check_program("dead cycles in a loop",
                  "fn make() {\n  let f = nil\n  f = fn() { return f }\n  return 0\n}\n"
                  "let i = 0\nwhile i < 500 {\n  make()\n  i = i + 1\n}\nprintln(\"ok\")\n", true);
    check_program("held, then dropped", TICK MAKE_SELF
                  "let i = 0\nwhile i < 300 {\n  let c = make()\n  c = nil\n  tick()\n  i = i + 1\n}\n"
                  "println(\"ok\")\n", true);
    check_program("held for a long time", TICK MAKE_SELF
                  "let keep = []\nlet i = 0\nwhile i < 50 {\n  push(keep, make())\n  i = i + 1\n}\n"
                  "let t = 0\nwhile t < 400 {\n  tick()\n  t = t + 1\n}\nprintln(len(keep))\n", true);
    check_program("closed after the upvalue closed", TICK
                  "fn make() {\n  let xs = []\n  return fn(f) {\n    push(xs, f)\n    return len(xs)\n  }\n}\n"
                  "let i = 0\nwhile i < 200 {\n  let add = make()\n  add(add)\n  add = nil\n  tick()\n  i = i + 1\n}\n"
                  "println(\"ok\")\n", true);
    check_program("mutual recursion",
                  "fn make() {\n  let even = nil\n  let odd = nil\n"
                  "  even = fn(n) {\n    if n == 0 { return true }\n    return odd(n - 1)\n  }\n"
                  "  odd = fn(n) {\n    if n == 0 { return false }\n    return even(n - 1)\n  }\n"
                  "  return even(10)\n}\nlet i = 0\nwhile i < 200 {\n  make()\n  i = i + 1\n}\nprintln(make())\n", true);
    check_program("block exits",
                  "let i = 0\nwhile i < 200 {\n  {\n    let f = nil\n    f = fn() { return f }\n  }\n  i = i + 1\n}\n"
                  "println(\"ok\")\n", true);
    check_program("over the budget of a tick (a full pass takes it)",
                  "fn make() {\n  let big = []\n  let i = 0\n  while i < 3000 {\n    push(big, [i, []])\n    i = i + 1\n  }\n  let f = nil\n  f = fn() {\n    len(big)\n    return f\n  }\n"
                  "  return 0\n}\nlet i = 0\nwhile i < 10 {\n  make()\n  i = i + 1\n}\nprintln(\"ok\")\n", true);
    check_program("dead cycles, then only allocations",
                  "fn make() {\n  let f = nil\n  f = fn() { return f }\n  return f\n}\n"
                  "let keep = []\nlet i = 0\nwhile i < 150 {\n  push(keep, make())\n  i = i + 1\n}\nkeep = nil\n"
                  "let j = 0\nlet n = 0\nwhile j < 20000 {\n  let a = [j, j, j]\n  n = n + len(a)\n  j = j + 1\n}\nprintln(n)\n",
                  true);
    check_program("a closure assigned to a closed upvalue", TICK
                  "fn make() {\n  let slot = nil\n  let c = fn(self) {\n    slot = self\n    return 0\n  }\n  return c\n}\n"
                  "let i = 0\nwhile i < 300 {\n  let c = make()\n  c(c)\n  c = nil\n  tick()\n  i = i + 1\n}\n"
                  "println(\"ok\")\n",
                  true);
    check_program("an array assigned to a closed upvalue", TICK
                  "fn make() {\n  let box = 0\n  let c = fn(self) {\n    box = [self]\n    return 0\n  }\n  return c\n}\n"
                  "let i = 0\nwhile i < 300 {\n  let c = make()\n  c(c)\n  c = nil\n  tick()\n  i = i + 1\n}\n"
                  "println(\"ok\")\n",
                  true);
    /* numbers stay on the JIT's inline path; strings, arrays and closures go to the reference */
    check_program("hot scalar and string assignments to a captured variable",
                  "fn make() {\n  let n = 0\n  let f = 0.5\n  let s = nil\n  return fn() {\n    n = n + 1\n"
                  "    f = f + 1.5\n    s = \"x\"\n    return n\n  }\n}\n"
                  "let c = make()\nlet i = 0\nwhile i < 3000 {\n  c()\n  i = i + 1\n}\nprintln(c())\n",
                  true);
    heap_limit = 3000000; /* big dead cycles under a limit: the growth and the limit passes must agree too */
    check_program("big dead cycles under the heap limit",
                  "fn make() {\n  let big = []\n  let i = 0\n  while i < 8000 {\n    push(big, [i, []])\n    i = i + 1\n  }\n"
                  "  let f = nil\n  f = fn() {\n    len(big)\n    return f\n  }\n  return f\n}\n"
                  "let r = 0\nlet n = 0\nwhile r < 6 {\n  let c = make()\n  c = nil\n  n = n + 1\n  r = r + 1\n}\nprintln(n)\n",
                  true);
    heap_limit = 700000; /* the heap limit asks for a collection: every engine must reach the same state */
    check_program("under the heap limit",
                  "fn make() {\n  let f = nil\n  f = fn() { return f }\n  return f\n}\n"
                  "let keep = []\nlet i = 0\nwhile i < 3000 {\n  push(keep, make())\n  i = i + 1\n}\nkeep = nil\n"
                  "let big = []\nlet j = 0\nwhile j < 4000 {\n  push(big, [j, j, j, j])\n  j = j + 1\n}\nprintln(len(big))\n",
                  true);
    heap_limit = 0;
    check_program("an error unwinds through closures",
                  "fn bad() {\n  let f = nil\n  f = fn() { return f }\n  return 1 / 0\n}\nbad()\n", true);
    return LZ_TEST_RESULT();
}
