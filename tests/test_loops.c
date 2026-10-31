#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "lizard/builtin.h"
#include "lizard/compiler.h"
#include "lizard/debug.h"
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

/* Fuel of 0 means unlimited. */
static void run(Program *program, const char *text, uint64_t fuel) {
    memset(program, 0, sizeof *program);
    lz_vm_init(&program->vm, lz_writer_from_buffer(&program->out),
               lz_writer_from_buffer(&program->err));
    LZ_CHECK(lz_builtin_install(&program->vm) == LZ_OK);

    lz_source_from_string(&program->source, "t.lz", text);
    lz_chunk_init(&program->chunk);
    program->compile_status =
        lz_compile(&program->vm, &program->source, &program->chunk, &program->diagnostics);

    if (fuel != 0) {
        lz_vm_set_fuel(&program->vm, fuel);
    }
    if (program->compile_status == LZ_OK) {
        program->run_status = lz_vm_run(&program->vm, &program->chunk);
    }
}

static void finish(Program *program) {
    LZ_CHECK(program->vm.stack_count == 0);
    lz_chunk_free(&program->vm.heap, &program->chunk);
    lz_vm_release_roots(&program->vm);
    LZ_CHECK(program->vm.heap.live_objects == 0);
    LZ_CHECK(program->vm.heap.live_bytes == 0);
    lz_vm_free(&program->vm);
    lz_buffer_free(&program->out);
    lz_buffer_free(&program->err);
}

static void check_output(const char *text, const char *expected) {
    Program program;
    run(&program, text, 0);
    LZ_CHECK(program.compile_status == LZ_OK);
    LZ_CHECK(program.run_status == LZ_OK);
    const char *actual = program.out.data != NULL ? program.out.data : "";
    bool same = strcmp(actual, expected) == 0;
    if (!same) {
        fprintf(stderr, "program:\n%s\nexpected: [%s]\ngot: [%s]\n", text, expected, actual);
        for (size_t i = 0; i < program.diagnostics.count; i++) {
            fprintf(stderr, "diagnostic: %s\n", program.diagnostics.items[i].message);
        }
        if (program.run_status != LZ_OK) {
            fprintf(stderr, "runtime: %s\n", program.vm.error);
        }
    }
    LZ_CHECK(same);
    finish(&program);
}

/* The program must not compile; the first error has this text on this line and column. */
static void check_compile_error(const char *text, const char *message, uint32_t line,
                                uint32_t column) {
    Program program;
    run(&program, text, 0);
    bool ok = program.compile_status == LZ_ERR_COMPILE && program.diagnostics.count > 0 &&
              strstr(program.diagnostics.items[0].message, message) != NULL;
    if (!ok) {
        fprintf(stderr, "program:\n%s\nexpected a compile error with [%s]; got status %d [%s]\n", text,
                message, (int)program.compile_status,
                program.diagnostics.count > 0 ? program.diagnostics.items[0].message : "no diagnostic");
    }
    LZ_CHECK(ok);
    if (ok) {
        LzLocation at = lz_source_locate(&program.source, program.diagnostics.items[0].offset);
        if (at.line != line || at.column != column) {
            fprintf(stderr, "error at %u:%u, expected %u:%u\n", (unsigned)at.line, (unsigned)at.column,
                    (unsigned)line, (unsigned)column);
        }
        LZ_CHECK(at.line == line && at.column == column);
        /* The caret covers the keyword itself, not the rest of the line. */
        const char *word = strchr(message + 1, '\'');
        if (word != NULL && strstr(message, "outside of a loop") != NULL) {
            LZ_CHECK(program.diagnostics.items[0].length == (uint32_t)(word - message - 1));
        }
    }
    finish(&program);
}

/* Runs with a fuel budget and expects it to run out at the given line. */
static void check_runs_out_of_fuel(const char *text, uint64_t fuel, uint32_t line) {
    Program program;
    run(&program, text, fuel);
    LZ_CHECK(program.compile_status == LZ_OK);
    bool ok = program.run_status == LZ_ERR_FUEL && strstr(program.vm.error, "instruction limit") != NULL;
    if (!ok) {
        fprintf(stderr, "program:\n%s\nexpected the fuel to run out; got %d [%s]\n", text,
                (int)program.run_status, program.vm.error);
    }
    LZ_CHECK(ok);
    if (ok && line != 0) {
        LzLocation at = lz_source_locate(&program.source, program.vm.error_offset);
        if (at.line != line) {
            fprintf(stderr, "error at line %u, expected %u\n", (unsigned)at.line, (unsigned)line);
        }
        LZ_CHECK(at.line == line);
    }
    finish(&program);
}

static void test_basic(void) {
    /* Count up to the first value the loop rejects. */
    check_output("let i = 0\nwhile i < 10 {\n i = i + 1\n if i == 4 { break }\n}\nprintln(i)\n", "4\n");
    /* Skip the even numbers. */
    check_output("let i = 0\nlet s = 0\nwhile i < 10 {\n i = i + 1\n if i % 2 == 0 { continue }\n s = s + i\n}\n"
                 "println(s)\n",
                 "25\n");
    /* The first element that matches, found from the loop. */
    check_output("let xs = [7, 8, 9, 10]\nlet k = 0\nwhile k < len(xs) {\n if xs[k] % 2 == 0 { break }\n k = k + 1\n}\n"
                 "println(k, xs[k])\n",
                 "1 8\n");
    check_output("let n = 0\nwhile true {\n n = n + 1\n break\n}\nprintln(n)\n", "1\n");
    /* A break in the last statement leaves nothing after it to run. */
    check_output("let i = 0\nwhile i < 3 {\n i = i + 1\n break\n println(\"never\", i)\n}\nprintln(i)\n", "1\n");
    /* break and continue as the only statements of an if branch. */
    check_output("let i = 0\nwhile i < 6 {\n i = i + 1\n if i < 3 { continue } else { break }\n}\nprintln(i)\n",
                 "3\n");
}

static void test_nested(void) {
    /* break leaves only the innermost loop. */
    check_output("let r = 0\nwhile r < 3 {\n let c = 0\n while c < 5 {\n  c = c + 1\n  if c == 2 { break }\n }\n"
                 " println(r, c)\n r = r + 1\n}\n",
                 "0 2\n1 2\n2 2\n");
    /* continue belongs to the innermost loop too. */
    check_output("let r = 0\nwhile r < 3 {\n let c = 0\n while c < 4 {\n  c = c + 1\n  if c == 2 { continue }\n"
                 "  println(r, c)\n }\n r = r + 1\n}\n",
                 "0 1\n0 3\n0 4\n1 1\n1 3\n1 4\n2 1\n2 3\n2 4\n");
    /* A break in the inner loop does not stop the outer one. */
    check_output("let i = 0\nlet hits = 0\nwhile i < 5 {\n i = i + 1\n let j = 0\n while true {\n  j = j + 1\n"
                 "  if j == 3 { break }\n }\n hits = hits + 1\n}\nprintln(hits)\n",
                 "5\n");
}

static void test_locals(void) {
    /* Locals declared in nested blocks of the body are gone when continue and break jump. */
    check_output("let i = 0\nlet s = 0\nwhile i < 6 {\n i = i + 1\n let a = i * 2\n { let b = a + 1\n"
                 "   if i == 2 { continue }\n   s = s + b }\n if i == 4 { break }\n let c = a\n s = s + c\n}\n"
                 "println(s, i)\n",
                 "27 4\n");
    /* Loop variable declared inside a body block is fine across iterations. */
    check_output("let i = 0\nwhile i < 3 {\n i = i + 1\n if i == 2 { continue }\n let x = i\n println(x)\n}\n",
                 "1\n3\n");
    /* Locals in an if branch: end_scope runs before the jump. */
    check_output("let i = 0\nlet s = 0\nwhile i < 5 {\n i = i + 1\n if i % 2 == 0 { let t = i * 10\n continue }\n"
                 " s = s + i\n}\nprintln(s)\n",
                 "9\n");
}

static void test_closures(void) {
    /* A closure made before a continue keeps the value of that iteration. */
    check_output("let i = 0\nlet a = nil\nlet b = nil\nlet c = nil\nwhile i < 5 {\n i = i + 1\n let v = i\n"
                 " let f = fn() { return v }\n if i == 2 { a = f\n continue }\n if i == 4 { b = f\n break }\n"
                 " if i == 3 { c = f }\n}\nprintln(a(), b(), c())\n",
                 "2 4 3\n");
    /* Many closures from many iterations: each keeps its own value. */
    check_output("let i = 0\nlet fs = []\nwhile i < 6 {\n i = i + 1\n let v = i * i\n push(fs, fn() { return v })\n"
                 " if i % 2 == 0 { continue }\n if i == 5 { break }\n}\nlet k = 0\nlet s = 0\n"
                 "while k < len(fs) {\n s = s + fs[k]()\n k = k + 1\n}\nprintln(len(fs), s)\n",
                 "5 55\n");
    /* A nested block's captured local, closed on the way out of the block by break. */
    check_output("let i = 0\nlet f = nil\nwhile i < 4 {\n i = i + 1\n { let w = i * 100\n f = fn() { return w }\n"
                 "   if i == 3 { break } }\n}\nprintln(f())\n",
                 "300\n");
}

static void test_return_in_loop(void) {
    /* return is still allowed inside a loop; the early exit is unchanged. */
    check_output("fn first_odd(xs) {\n let k = 0\n while k < len(xs) {\n  if xs[k] % 2 == 1 { return xs[k] }\n"
                 "  k = k + 1\n }\n return -1\n}\nprintln(first_odd([2, 4, 5, 7]), first_odd([2]))\n",
                 "5 -1\n");
    /* A loop whose every iteration breaks does not make the code after it unreachable. */
    check_output("fn f() -> int {\n let i = 0\n while true {\n  i = i + 1\n  if i > 0 { break } else { break }\n }\n"
                 " return i\n}\nprintln(f())\n",
                 "1\n");
    /* break in both branches of an if: still not a return, so the trailing return is required. */
    check_output("fn f(n: int) -> int {\n while true {\n  if n > 0 { break } else { break }\n }\n return n\n}\n"
                 "println(f(3))\n",
                 "3\n");
}

static void test_terminated(void) {
    /* The loop body ends in a break, so nothing below the loop in the body runs. */
    check_output("let i = 0\nwhile i < 3 {\n i = i + 1\n if i == 2 { break }\n}\nprintln(\"after\", i)\n",
                 "after 2\n");
    /* A trailing break is not a missing return. */
    Program program;
    const char *text = "fn f() -> int {\n let i = 0\n while true {\n  i = i + 1\n  if i > 2 { break }\n }\n"
                       " return i\n}\nprintln(f())\n";
    run(&program, text, 0);
    LZ_CHECK(program.compile_status == LZ_OK);
    LZ_CHECK(program.run_status == LZ_OK);
    LZ_CHECK(strcmp(program.out.data != NULL ? program.out.data : "", "3\n") == 0);
    finish(&program);
}

static void test_fuel(void) {
    /* continue loops back through a back-edge, so the loop can still be stopped. */
    check_runs_out_of_fuel("while true {\n continue\n}\n", 500, 1);
    check_runs_out_of_fuel("let i = 0\nwhile true {\n i = i + 1\n if i > 10 { continue }\n i = 0\n}\n", 5000, 3);
    /* A tiny budget still ends, and the error points into the loop. */
    check_runs_out_of_fuel("let i = 0\nwhile i < 1000000 {\n i = i + 1\n if i % 2 == 0 { continue }\n}\n", 200, 4);
}

static void test_compile_errors(void) {
    check_compile_error("break\n", "'break' outside of a loop", 1, 1);
    check_compile_error("continue\n", "'continue' outside of a loop", 1, 1);
    check_compile_error("let i = 0\nwhile i < 3 {\n i = i + 1\n}\nbreak\n", "'break' outside of a loop", 5, 1);
    check_compile_error("if true {\n continue\n}\n", "'continue' outside of a loop", 2, 2);
    check_compile_error("fn f() {\n break\n}\n", "'break' outside of a loop", 2, 2);
    /* A function or lambda inside a loop cannot reach it. */
    check_compile_error("while true {\n let f = fn() { break }\n f()\n}\n",
                        "'break' outside of a loop (a function body is not part of the loop)", 2, 17);
    check_compile_error("while true {\n fn g() { continue }\n}\n",
                        "'continue' outside of a loop (a function body is not part of the loop)", 2, 11);
    check_compile_error("while true {\n let h = fn(x) { continue }\n}\n",
                        "'continue' outside of a loop (a function body is not part of the loop)", 2, 18);
    /* The keyword is a keyword: it cannot be a variable or a function name. */
    check_compile_error("let break = 1\n", "expected variable name after 'let'", 1, 5);
    check_compile_error("let continue = 1\n", "expected variable name after 'let'", 1, 5);
    check_compile_error("fn break() { }\n", "expected '(' after 'fn'", 1, 4);
    check_compile_error("let x = break\n", "expected expression", 1, 9);
}

static void test_identifiers(void) {
    /* Names that merely start like the keywords stay valid. */
    check_output("let breaker = 1\nlet continued = 2\nlet breaking = 3\nprintln(breaker, continued, breaking)\n",
                 "1 2 3\n");
    check_output("fn continuex() { return 4 }\nprintln(continuex())\n", "4\n");
    check_output("let i = 0\nwhile i < 2 {\n i = i + 1\n let breaks = i\n println(breaks)\n}\n", "1\n2\n");
}

int main(void) {
    test_basic();
    test_nested();
    test_locals();
    test_closures();
    test_return_in_loop();
    test_terminated();
    test_fuel();
    test_compile_errors();
    test_identifiers();
    return LZ_TEST_RESULT();
}
