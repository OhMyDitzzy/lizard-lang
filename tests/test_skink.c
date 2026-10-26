#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "lizard/builtin.h"
#include "lizard/compiler.h"
#include "lizard/skink.h"
#include "lizard/vm.h"
#include "test_util.h"

typedef struct Program {
    LzVM vm;
    LzBuffer out;
    LzBuffer err;
    LzChunk chunk;
    LzDiagnostics diagnostics;
    LzSource source;
    LzStatus compile_status;
    LzStatus run_status;
} Program;

/* Heap limit for the next run, 0 for none; applied after the builtins, as the CLI does. */
static size_t heap_limit = 0;

static void run(Program *program, const char *text, bool skink) {
    memset(program, 0, sizeof *program);
    lz_vm_init(&program->vm, lz_writer_from_buffer(&program->out),
               lz_writer_from_buffer(&program->err));
    program->vm.skink.enabled = skink;
    LZ_CHECK(lz_builtin_install(&program->vm) == LZ_OK);
    if (heap_limit != 0) {
        lz_vm_set_heap_limit(&program->vm, heap_limit);
    }
    lz_source_from_string(&program->source, "t.lz", text);
    lz_chunk_init(&program->chunk);
    program->compile_status =
        lz_compile(&program->vm, &program->source, &program->chunk, &program->diagnostics);
    if (program->compile_status == LZ_OK) {
        program->run_status = lz_vm_run(&program->vm, &program->chunk);
    }
}

/* Returns what was still alive once the roots were released: a leaked cycle, if any. */
static size_t finish(Program *program) {
    LZ_CHECK(program->vm.stack_count == 0);
    LZ_CHECK(program->vm.skink.pending_count == 0);
    LZ_CHECK(program->vm.skink.stats.inconsistent == 0);
    lz_chunk_free(&program->vm.heap, &program->chunk);
    lz_vm_release_roots(&program->vm);
    size_t leaked = program->vm.heap.live_objects;
    lz_vm_free(&program->vm);
    lz_buffer_free(&program->out);
    lz_buffer_free(&program->err);
    return leaked;
}

static bool output_is(const Program *program, const char *expected) {
    const char *actual = program->out.data != NULL ? program->out.data : "";
    bool same = strcmp(actual, expected) == 0;
    if (!same) {
        fprintf(stderr, "expected [%s], got [%s] (%s)\n", expected, actual, program->vm.error);
    }
    return same;
}

/* A program whose closures form dead cycles: Skink must free them and the output must not change. */
static void check_freed(const char *text, const char *expected, uint64_t cycles) {
    Program on;
    run(&on, text, true);
    LZ_CHECK(on.compile_status == LZ_OK);
    LZ_CHECK(on.run_status == LZ_OK);
    LZ_CHECK(output_is(&on, expected));
    LZ_CHECK(on.vm.skink.stats.cycles == cycles);
    LZ_CHECK(finish(&on) == 0);

    Program off; /* the same program leaks without Skink, so the check above means something */
    run(&off, text, false);
    LZ_CHECK(output_is(&off, expected));
    LZ_CHECK(finish(&off) > 0);
}

static void test_closure_that_captures_itself(void) {
    check_freed("fn make() {\n  let f = nil\n  f = fn() { return f }\n  return 0\n}\n"
                "make()\nprintln(\"done\")\n",
                "done\n", 1);
}

static void test_array_and_closure_hold_each_other(void) {
    check_freed("fn make() {\n  let xs = []\n  push(xs, fn() { return xs })\n  return len(xs)\n}\n"
                "println(make())\n",
                "1\n", 1);
}

static void test_mutual_recursion_has_two_suspects_and_one_cycle(void) {
    check_freed("fn make() {\n"
                "  let even = nil\n  let odd = nil\n"
                "  even = fn(n) {\n    if n == 0 { return true }\n    return odd(n - 1)\n  }\n"
                "  odd = fn(n) {\n    if n == 0 { return false }\n    return even(n - 1)\n  }\n"
                "  return even(10)\n}\n"
                "println(make())\n",
                "true\n", 1);
}

static void test_block_exit_closes_upvalues_too(void) {
    check_freed("{\n  let f = nil\n  f = fn() { return f }\n}\nprintln(\"block\")\n", "block\n", 1);
}

/* A function whose frame closes an upvalue: every return from it is a tick of the watch lists. */
#define TICK_FN "fn tick() {\n  let k = 0\n  let g = fn() { return k }\n  return 0\n}\n"

static void test_a_cycle_that_is_still_held_is_not_freed(void) {
    Program program;
    run(&program, "fn make() {\n  let f = nil\n  let n = 0\n  f = fn() {\n    n = n + 1\n"
                  "    if n >= 3 { return n }\n    return f\n  }\n  return f\n}\n"
                  "let c = make()\nlet a = c()\nlet b = a()\nprintln(b())\n",
        true);
    LZ_CHECK(program.run_status == LZ_OK);
    LZ_CHECK(output_is(&program, "3\n"));
    LZ_CHECK(program.vm.skink.stats.cycles == 0); /* c holds it: nothing may be freed */
    LZ_CHECK(program.vm.skink.stats.watched == 1);
    LZ_CHECK(finish(&program) == 0); /* the sweep once the globals are gone frees it */
}

static void test_a_cycle_released_after_the_frame_exit_is_found_by_a_later_tick(void) {
    Program program;
    run(&program, TICK_FN "fn make() {\n  let f = nil\n  f = fn() { return f }\n  return f\n}\n"
                          "let c = make()\nc = nil\ntick()\nprintln(\"ok\")\n",
        true);
    LZ_CHECK(program.run_status == LZ_OK);
    LZ_CHECK(output_is(&program, "ok\n"));
    LZ_CHECK(program.vm.skink.stats.polls >= 1);
    LZ_CHECK(program.vm.skink.stats.cycles == 1); /* freed during the run, not by the final sweep */
    LZ_CHECK(finish(&program) == 0);
}

static void test_a_cycle_closed_after_its_upvalue_was_closed(void) {
    Program program;
    run(&program, TICK_FN "fn make() {\n  let xs = []\n  return fn(f) {\n    push(xs, f)\n"
                          "    return len(xs)\n  }\n}\n"
                          "let add = make()\nprintln(add(add))\nadd = nil\ntick()\n",
        true);
    LZ_CHECK(program.run_status == LZ_OK);
    LZ_CHECK(output_is(&program, "1\n"));
    LZ_CHECK(program.vm.skink.stats.cycles == 1); /* closure -> upvalue -> array -> closure */
    LZ_CHECK(finish(&program) == 0);
}

static void test_a_live_watched_structure_is_polled_ever_less_often(void) {
    Program program;
    run(&program, TICK_FN "fn make() {\n  let f = nil\n  f = fn() { return f }\n  return f\n}\n"
                          "let keep = make()\nlet i = 0\nwhile i < 1000 {\n  tick()\n  i = i + 1\n}\n"
                          "println(\"ok\")\n",
        true);
    LZ_CHECK(program.run_status == LZ_OK);
    LZ_CHECK(program.vm.skink.stats.cycles == 0);
    LZ_CHECK(program.vm.skink.stats.polls >= 4);
    LZ_CHECK(program.vm.skink.stats.polls < 40); /* 1000 ticks, backed off to every 64th */
    LZ_CHECK(finish(&program) == 0);
}

static void test_no_tick_is_ever_more_than_a_bounded_amount_of_work(void) {
    Program program;
    run(&program, TICK_FN "fn make() {\n  let f = nil\n  f = fn() { return f }\n  return f\n}\n"
                          "let keep = []\nlet i = 0\nwhile i < 200 {\n  push(keep, make())\n  i = i + 1\n}\n"
                          "let t = 0\nwhile t < 100 {\n  tick()\n  t = t + 1\n}\nprintln(len(keep))\n",
        true);
    LZ_CHECK(program.run_status == LZ_OK);
    LZ_CHECK(output_is(&program, "200\n"));
    /* 200 make() exits and 100 tick() exits, at most LZ_SKINK_POLLS_PER_TICK polls each */
    LZ_CHECK(program.vm.skink.stats.polls <= 300 * LZ_SKINK_POLLS_PER_TICK);
    LZ_CHECK(finish(&program) == 0);
}

#define ASSIGN_MAKE(VALUE) "fn make() {\n  let slot = nil\n  let c = fn(self) {\n    slot = " VALUE "\n    return 0\n  }\n  return c\n}\n"

static void test_an_object_assigned_to_a_closed_upvalue_is_watched(void) {
    /* slot held nil when make() returned, so nothing reported it; c(c) then closes the cycle. */
    const char *text = TICK_FN ASSIGN_MAKE("self") "let c = make()\nc(c)\nc = nil\ntick()\nprintln(\"ok\")\n";
    Program on;
    run(&on, text, true);
    LZ_CHECK(on.run_status == LZ_OK);
    LZ_CHECK(output_is(&on, "ok\n"));
    LZ_CHECK(on.vm.skink.stats.stored == 1);
    LZ_CHECK(on.vm.skink.stats.cycles == 1); /* freed by the tick, not by the final sweep */
    LZ_CHECK(finish(&on) == 0);

    Program off; /* nothing else would ever find it */
    run(&off, text, false);
    LZ_CHECK(finish(&off) > 0);
}

static void test_an_array_assigned_to_a_closed_upvalue_is_watched(void) {
    const char *text = TICK_FN ASSIGN_MAKE("[self]") "let c = make()\nc(c)\nc = nil\ntick()\nprintln(\"ok\")\n";
    Program program;
    run(&program, text, true);
    LZ_CHECK(program.run_status == LZ_OK);
    LZ_CHECK(program.vm.skink.stats.stored == 1);
    LZ_CHECK(program.vm.skink.stats.cycles == 1); /* closure -> upvalue -> array -> closure */
    LZ_CHECK(finish(&program) == 0);
}

static void test_numbers_and_strings_assigned_to_a_closed_upvalue_cost_nothing(void) {
    Program program;
    run(&program, "fn make() {\n  let n = 0\n  let s = nil\n  return fn() {\n    n = n + 1\n    s = \"x\"\n"
                  "    return n\n  }\n}\n"
                  "let c = make()\nlet i = 0\nwhile i < 100 {\n  c()\n  i = i + 1\n}\nprintln(c())\n",
        true);
    LZ_CHECK(output_is(&program, "101\n"));
    LZ_CHECK(program.vm.skink.stats.stored == 0); /* a string cannot close a cycle either */
    LZ_CHECK(program.vm.skink.stats.watched == 0);
    LZ_CHECK(finish(&program) == 0);
}

static void test_an_assignment_to_an_open_upvalue_is_the_close_s_business(void) {
    /* While make() runs, slot is open: the store goes to the stack slot, and the close reports it. */
    Program program;
    run(&program, "fn make() {\n  let slot = nil\n  let f = fn() { return slot }\n  slot = f\n  return 0\n}\n"
                  "make()\nprintln(\"ok\")\n",
        true);
    LZ_CHECK(program.run_status == LZ_OK);
    LZ_CHECK(program.vm.skink.stats.stored == 0);
    LZ_CHECK(program.vm.skink.stats.cycles == 1);
    LZ_CHECK(finish(&program) == 0);
}

/* No upvalue closes after keep is dropped, so no frame exit ticks: only allocations can. */
static void test_dead_cycles_are_found_by_allocation_ticks_alone(void) {
    Program program;
    run(&program, "fn make() {\n  let f = nil\n  f = fn() { return f }\n  return f\n}\n"
                  "let keep = []\nlet i = 0\nwhile i < 200 {\n  push(keep, make())\n  i = i + 1\n}\n"
                  "keep = nil\nlet j = 0\nlet n = 0\nwhile j < 60000 {\n  let a = [j, j, j]\n  n = n + len(a)\n  j = j + 1\n}\n"
                  "println(n)\n",
        true);
    LZ_CHECK(program.run_status == LZ_OK);
    LZ_CHECK(output_is(&program, "180000\n"));
    LZ_CHECK(program.vm.skink.stats.cycles == 200);  /* all of them, before the final sweep */
    LZ_CHECK(program.vm.skink.stats.alloc_ticks > 0);
    LZ_CHECK(finish(&program) == 0);
}

/* How many upvalues one captured variable gets reported as a suspect, and what the program printed. */
static uint64_t suspects_of(const char *text, const char *expected_output) {
    Program program;
    run(&program, text, true);
    LZ_CHECK(program.run_status == LZ_OK);
    LZ_CHECK(output_is(&program, expected_output));
    uint64_t suspects = program.vm.skink.stats.suspects;
    LZ_CHECK(finish(&program) == 0);
    return suspects;
}

#define CAPTURE(DECL, EXPR)                                                                        \
    "fn make() {\n  " DECL "\n  return fn() { return " EXPR " }\n}\nlet c = make()\nprintln(c())\n"

/*
 * A typed array whose items can never be a function cannot reach a closure, however deeply
 * it nests, so it cannot be on a cycle and is never a suspect.
 */
static void test_arrays_that_cannot_reach_a_closure_are_never_suspects(void) {
    LZ_CHECK(suspects_of(CAPTURE("let m: int[] = [1, 2]", "m[1]"), "2\n") == 0);
    LZ_CHECK(suspects_of(CAPTURE("let m: int[][] = [[1, 2], [3, 4]]", "m[1][0]"), "3\n") == 0);
    LZ_CHECK(suspects_of(CAPTURE("let m: string[][][] = [[[\"a\"]]]", "m[0][0][0]"), "a\n") == 0);
    LZ_CHECK(suspects_of(CAPTURE("let m: int?[][] = [[1, nil]]", "m[0][0]"), "1\n") == 0);
    LZ_CHECK(suspects_of(CAPTURE("let m: (int | string)[][] = [[1, \"x\"]]", "m[0][1]"), "x\n") == 0);
}

static void test_arrays_that_might_hold_a_closure_are_still_suspects(void) {
    LZ_CHECK(suspects_of(CAPTURE("let m: any[] = [[1, 2], [3, 4]]", "m[1][0]"), "3\n") == 1);
    LZ_CHECK(suspects_of(CAPTURE("let m: any[][] = [[1, 2], [3, 4]]", "m[1][0]"), "3\n") == 1);
    LZ_CHECK(suspects_of(CAPTURE("let m = [[1, 2], [3, 4]]", "m[1][0]"), "3\n") == 1); /* unannotated */
}

static void test_memory_stays_flat_in_a_loop(void) {
    const char *text = "fn make() {\n  let f = nil\n  f = fn() { return f }\n  return 0\n}\n"
                       "let i = 0\nwhile i < 3000 {\n  make()\n  i = i + 1\n}\nprintln(\"ok\")\n";
    Program on;
    run(&on, text, true);
    LZ_CHECK(on.run_status == LZ_OK);
    LZ_CHECK(on.vm.skink.stats.cycles == 3000);
    LZ_CHECK(on.vm.heap.peak_bytes < 16 * 1024);
    LZ_CHECK(finish(&on) == 0);

    Program off;
    run(&off, text, false);
    LZ_CHECK(off.vm.heap.peak_bytes > 100 * 1024);
    (void)finish(&off);
}

static void test_a_subgraph_over_the_budget_is_left_alone(void) {
    Program program;
    run(&program, "fn make() {\n  let big = range(20000)\n  let f = nil\n"
                  "  f = fn() {\n    len(big)\n    return f\n  }\n  return 0\n}\n"
                  "make()\nprintln(\"big\")\n",
        true);
    LZ_CHECK(program.run_status == LZ_OK);
    LZ_CHECK(output_is(&program, "big\n"));
    LZ_CHECK(program.vm.skink.stats.over_budget > 0);
    LZ_CHECK(program.vm.skink.stats.cycles == 0);
    (void)finish(&program);
}

static void test_closures_over_numbers_and_typed_arrays_are_never_suspects(void) {
    Program program;
    run(&program, "fn make() {\n  let xs: int[] = [1, 2, 3]\n  let n = 0\n"
                  "  return fn() {\n    n = n + len(xs)\n    return n\n  }\n}\n"
                  "let c = make()\nc()\nprintln(c())\n",
        true);
    LZ_CHECK(output_is(&program, "6\n"));
    LZ_CHECK(program.vm.skink.stats.suspects == 0);
    LZ_CHECK(finish(&program) == 0);
}

static void test_an_error_leaves_no_pending_suspects(void) {
    Program program;
    run(&program, "fn bad() {\n  let f = nil\n  f = fn() { return f }\n  return 1 / 0\n}\nbad()\n",
        true);
    LZ_CHECK(program.run_status != LZ_OK);
    (void)finish(&program); /* finish checks that the pending list is empty */
}

/* Dead cycles that wait for their next poll (no upvalue closes after keep is dropped) while the
 * program needs the room: the heap limit must trigger a collection, not an error. */
#define PRESSURE_MAKE "fn make() {\n  let f = nil\n  f = fn() { return f }\n  return f\n}\n"
#define PRESSURE_FILL "let big = []\nlet j = 0\nwhile j < 4000 {\n  push(big, [j, j, j, j])\n  j = j + 1\n}\nprintln(len(big))\n"

static void test_the_heap_limit_triggers_a_collection_before_it_refuses(void) {
    const char *text = PRESSURE_MAKE "let keep = []\nlet i = 0\nwhile i < 3000 {\n  push(keep, make())\n"
                                     "  i = i + 1\n}\nkeep = nil\n" PRESSURE_FILL;
    heap_limit = 700000;

    Program off;
    run(&off, text, false);
    LZ_CHECK(off.run_status == LZ_ERR_HEAP_LIMIT); /* without Skink the program dies */
    (void)finish(&off);

    Program on;
    run(&on, text, true);
    LZ_CHECK(on.run_status == LZ_OK);
    LZ_CHECK(output_is(&on, "4000\n"));
    LZ_CHECK(on.vm.skink.stats.reclaims >= 1);
    LZ_CHECK(on.vm.skink.stats.reclaimed > 300000);
    LZ_CHECK(finish(&on) == 0);

    heap_limit = 550000; /* the live data alone needs about 645 KB: no false rescue */
    Program too_small;
    run(&too_small, text, true);
    LZ_CHECK(too_small.run_status == LZ_ERR_HEAP_LIMIT);
    (void)finish(&too_small);
    heap_limit = 0;
}

static void test_the_emergency_pass_has_a_bigger_budget_than_a_tick(void) {
    const char *text = "fn make() {\n  let big = range(20000)\n  let f = nil\n  f = fn() {\n    len(big)\n"
                       "    return f\n  }\n  return f\n}\n"
                       "let keep = []\npush(keep, make())\npush(keep, make())\npush(keep, make())\nkeep = nil\n"
                       "let more = []\nlet j = 0\nwhile j < 4000 {\n  push(more, [j, j, j, j])\n  j = j + 1\n}\n"
                       "println(len(more))\n";
    heap_limit = 1400000;
    Program program;
    run(&program, text, true);
    LZ_CHECK(program.run_status == LZ_OK);
    LZ_CHECK(output_is(&program, "4000\n"));
    LZ_CHECK(program.vm.skink.stats.over_budget > 0);       /* the ordinary trials gave up on them */
    LZ_CHECK(program.vm.skink.stats.reclaimed > 900000);    /* the emergency pass did not */
    LZ_CHECK(finish(&program) == 0);
    heap_limit = 0;
}

static void test_more_suspects_than_the_pending_list_holds_are_still_watched(void) {
    /* 80 captured closures in one frame: past LZ_SKINK_PENDING_MAX, so they go straight to the watch list */
    char text[4096];
    size_t n = 0;
    n += (size_t)snprintf(text + n, sizeof text - n, "fn make() {\n");
    for (int i = 0; i < 80; i++) {
        n += (size_t)snprintf(text + n, sizeof text - n, "  let f%d = nil\n  f%d = fn() { return f%d }\n", i, i, i);
    }
    n += (size_t)snprintf(text + n, sizeof text - n, "  return 0\n}\nmake()\nprintln(\"ok\")\n");
    Program program;
    run(&program, text, true);
    LZ_CHECK(program.run_status == LZ_OK);
    LZ_CHECK(program.vm.skink.stats.dropped > 0);
    LZ_CHECK(finish(&program) == 0); /* the final sweep finds the ones no trial saw */
}

int main(void) {
    test_closure_that_captures_itself();
    test_array_and_closure_hold_each_other();
    test_mutual_recursion_has_two_suspects_and_one_cycle();
    test_block_exit_closes_upvalues_too();
    test_a_cycle_that_is_still_held_is_not_freed();
    test_a_cycle_released_after_the_frame_exit_is_found_by_a_later_tick();
    test_a_cycle_closed_after_its_upvalue_was_closed();
    test_a_live_watched_structure_is_polled_ever_less_often();
    test_no_tick_is_ever_more_than_a_bounded_amount_of_work();
    test_memory_stays_flat_in_a_loop();
    test_a_subgraph_over_the_budget_is_left_alone();
    test_closures_over_numbers_and_typed_arrays_are_never_suspects();
    test_an_error_leaves_no_pending_suspects();
    test_an_object_assigned_to_a_closed_upvalue_is_watched();
    test_an_array_assigned_to_a_closed_upvalue_is_watched();
    test_numbers_and_strings_assigned_to_a_closed_upvalue_cost_nothing();
    test_an_assignment_to_an_open_upvalue_is_the_close_s_business();
    test_dead_cycles_are_found_by_allocation_ticks_alone();
    test_arrays_that_cannot_reach_a_closure_are_never_suspects();
    test_arrays_that_might_hold_a_closure_are_still_suspects();
    test_the_heap_limit_triggers_a_collection_before_it_refuses();
    test_the_emergency_pass_has_a_bigger_budget_than_a_tick();
    test_more_suspects_than_the_pending_list_holds_are_still_watched();
    return LZ_TEST_RESULT();
}
