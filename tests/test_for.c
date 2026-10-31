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

/* The program must not compile; the first error carries this text at this position. */
static void check_compile_error(const char *text, const char *message, uint32_t line,
                                uint32_t column) {
    Program program;
    run(&program, text, 0);
    bool ok = program.compile_status == LZ_ERR_COMPILE && program.diagnostics.count > 0 &&
              strstr(program.diagnostics.items[0].message, message) != NULL;
    if (!ok) {
        fprintf(stderr, "program:\n%s\nexpected a compile error with [%s]; got status %d [%s]\n",
                text, message, (int)program.compile_status,
                program.diagnostics.count > 0 ? program.diagnostics.items[0].message
                                              : "no diagnostic");
    }
    LZ_CHECK(ok);
    if (ok) {
        LzLocation at = lz_source_locate(&program.source, program.diagnostics.items[0].offset);
        if (at.line != line || at.column != column) {
            fprintf(stderr, "error at %u:%u, expected %u:%u\n", (unsigned)at.line,
                    (unsigned)at.column, (unsigned)line, (unsigned)column);
        }
        LZ_CHECK(at.line == line && at.column == column);
    }
    finish(&program);
}

/* A run-time failure with this message. */
static void check_run_error(const char *text, const char *message) {
    Program program;
    run(&program, text, 0);
    LZ_CHECK(program.compile_status == LZ_OK);
    bool ok = program.run_status != LZ_OK && strstr(program.vm.error, message) != NULL;
    if (!ok) {
        fprintf(stderr, "program:\n%s\nexpected a run-time error with [%s]; got %d [%s]\n", text,
                message, (int)program.run_status, program.vm.error);
    }
    LZ_CHECK(ok);
    finish(&program);
}

/* Runs with a fuel budget and expects it to run out. */
static void check_runs_out_of_fuel(const char *text, uint64_t fuel) {
    Program program;
    run(&program, text, fuel);
    LZ_CHECK(program.compile_status == LZ_OK);
    bool ok = program.run_status == LZ_ERR_FUEL &&
              strstr(program.vm.error, "instruction limit") != NULL;
    if (!ok) {
        fprintf(stderr, "program:\n%s\nexpected the fuel to run out; got %d [%s]\n", text,
                (int)program.run_status, program.vm.error);
    }
    LZ_CHECK(ok);
    finish(&program);
}

static void test_arrays(void) {
    check_output("for x in [1, 2, 3] { println(x) }\n", "1\n2\n3\n");
    check_output("for x in [] { println(x) }\n", "");
    check_output("for i, x in [\"a\", \"b\"] { println(i, x) }\n", "0 a\n1 b\n");
    check_output("let xs = [1, 2, 3]\nfor x in xs { println(x * 2) }\n", "2\n4\n6\n");
    /* The body may change the array: the length is read again every round. */
    check_output("let xs = [1]\nfor x in xs {\n if xs.len() < 4 { xs.push(0) }\n println(x)\n}\n",
                 "1\n0\n0\n0\n");
    check_output("let xs = [1, 2, 3, 4]\nfor x in xs {\n println(x)\n if xs.len() > 1 { pop(xs) }\n}\n",
                 "1\n2\n");
    /* Nested loops over the same array. */
    check_output("for a in [1, 2] {\n for b in [3, 4] { println(a, b) }\n}\n",
                 "1 3\n1 4\n2 3\n2 4\n");
    /* The loop variable is fresh each round. */
    check_output("let gs = []\nfor x in [7, 8] { push(gs, fn() { return x }) }\n"
                 "println(gs[0](), gs[1]())\n",
                 "7 8\n");
    /* The item is the value itself: no copy is made for the array. */
    check_output("let xs = [[1], [2]]\nfor x in xs { push(x, 9) }\nprintln(xs)\n",
                 "[[1, 9], [2, 9]]\n");
}

static void test_strings(void) {
    check_output("for ch in \"abc\" { print(ch) }\nprintln()\n", "abc\n");
    check_output("for ch in \"\" { println(\"never\") }\n", "");
    /* One item per code point, not per byte. */
    check_output("for ch in \"héllo\" { print(ch) }\nprintln()\n", "héllo\n");
    check_output("for ch in \"✓✔\" { print(ch, \".\") }\nprintln()\n", "✓ .✔ .\n");
    check_output("for i, ch in \"hé\" { println(i, ch) }\n", "0 h\n1 é\n");
    check_output("let n = 0\nfor ch in \"abcd\" { n += 1 }\nprintln(n)\n", "4\n");
    check_output("for ch in \"abc\" {\n if ch == \"b\" { continue }\n print(ch)\n}\nprintln()\n",
                 "ac\n");
}

static void test_ranges(void) {
    check_output("for i in 0..3 { println(i) }\n", "0\n1\n2\n");
    check_output("for i in 0..=3 { println(i) }\n", "0\n1\n2\n3\n");
    check_output("for i in 3..0 { println(i) }\n", "");
    check_output("for i in 3..=0 { println(i) }\n", "");
    check_output("for i in 5..5 { println(i) }\n", "");
    check_output("for i in 5..=5 { println(i) }\n", "5\n");
    check_output("for i in 10..0 by -1 { print(i) }\nprintln()\n", "10987654321\n");
    check_output("for i in 0..10 by 3 { print(i) }\nprintln()\n", "0369\n");
    check_output("for i in 10..0 by -3 { print(i) }\nprintln()\n", "10741\n");
    check_output("for i in 10..=0 by -3 { print(i) }\nprintln()\n", "10741\n");
    /* The bounds and the step are evaluated once. */
    check_output("let n = 0\nfn bump() -> int { n += 1\n return 2 }\n"
                 "for i in 0..bump() { print(i) }\nprintln(n)\n",
                 "011\n");
    /* The top of the range is reachable: the counter never has to go past it. */
    check_output("for i in 9223372036854775806..=9223372036854775807 { print(i, \" \") }\nprintln()\n",
                 "9223372036854775806  9223372036854775807  \n");
    check_output("for i in 9223372036854775806..9223372036854775807 { print(i) }\nprintln()\n",
                 "9223372036854775806\n");
    check_output("let c = 0\nfor i in 9223372036854775800..=9223372036854775807 { c += 1 }\n"
                 "println(c)\n",
                 "8\n");
    /* A step far larger than the range still visits both ends safely. */
    check_output("for i in 9223372036854775807..0 by -9223372036854775807 { print(i, \" \") }\n"
                 "println()\n",
                 "9223372036854775807  \n");
    check_output("for i in 0..=-9223372036854775807 by -9223372036854775807 { print(i, \" \") }\n"
                 "println()\n",
                 "0  -9223372036854775807  \n");
    check_output("for i in -9223372036854775807..0 by 9223372036854775807 { print(i, \" \") }\n"
                 "println()\n",
                 "-9223372036854775807  \n");
    /* The empty side of the extremes. */
    check_output("for i in 9223372036854775807..=9223372036854775806 { print(i) }\nprintln()\n", "\n");
    check_output("for i in -9223372036854775807..-9223372036854775807 { print(i) }\nprintln()\n", "\n");
}

static void test_c_style(void) {
    check_output("for let i = 0; i < 3; i += 1 { print(i) }\nprintln()\n", "012\n");
    check_output("for let i = 1; i < 100; i *= 2 { print(i) }\nprintln()\n", "1248163264\n");
    check_output("for let i = 0, j = 4; i < j; i += 1, j -= 1 { println(i, j) }\n",
                 "0 4\n1 3\n");
    check_output("for let i: int = 0; i < 2; i += 1 { print(i) }\nprintln()\n", "01\n");
    check_output("for let i = 0; i < 2; i += 1 { }\nprintln(\"done\")\n", "done\n");
    check_output("for let i = 0; false; i += 1 { println(\"never\") }\nprintln(\"after\")\n",
                 "after\n");
    check_output("let n = 0\nfor ;; {\n n += 1\n if n == 3 { break }\n}\nprintln(n)\n", "3\n");
    /* continue still runs the step. */
    check_output("for let i = 0; i < 5; i += 1 {\n if i % 2 == 0 { continue }\n print(i)\n}\nprintln()\n",
                 "13\n");
    /* The loop variable is an ordinary local: the body may assign it. */
    check_output("let s = 0\nfor let i = 0; i < 6; i += 1 {\n s += i\n i += 1\n}\nprintln(s)\n", "6\n");
    /* Fresh per round for closures. */
    check_output("let fs = []\nfor let i = 0; i < 3; i += 1 { push(fs, fn() { return i }) }\n"
                 "println(fs[0](), fs[1](), fs[2]())\n",
                 "0 1 2\n");
    check_output("let fs = []\nfor let i = 0; i < 5; i += 1 {\n if i % 2 == 0 { continue }\n"
                 " push(fs, fn() { return i })\n}\nprintln(fs[0](), fs[1]())\n",
                 "1 3\n");
    /* A function call is a step. */
    check_output("let n = 0\nfn tick() -> int { n += 1\n return n }\n"
                 "for let i = 0; i < 3; tick() { i += 1 }\nprintln(n)\n",
                 "3\n");
    /* The variables live only inside the loop. */
    check_run_error("for let i = 0; i < 2; i += 1 { }\nprintln(i)\n", "undefined variable 'i'");
    check_run_error("for x in [1] { }\nprintln(x)\n", "undefined variable 'x'");
}

static void test_nested_and_scoped(void) {
    check_output("for i in 0..2 {\n for j in 0..2 {\n  if j == 1 { continue }\n  println(i, j)\n }\n}\n",
                 "0 0\n1 0\n");
    check_output("for i in 0..3 {\n if i == 1 { continue }\n if i == 2 { break }\n println(i)\n}\n", "0\n");
    check_output("for let i = 0; i < 3; i += 1 {\n for let j = 0; j < 3; j += 1 {\n"
                 "  if j == 1 { break }\n  println(i, j)\n }\n}\n",
                 "0 0\n1 0\n2 0\n");
    /* A loop inside a function and inside a lambda. */
    check_output("fn total(xs: int[]) -> int {\n let t = 0\n for x in xs { t += x }\n return t\n}\n"
                 "println(total([1, 2, 3]))\n",
                 "6\n");
    check_output("let f = fn(n: int) -> int {\n let t = 0\n for i in 1..=n { t += i }\n return t\n}\n"
                 "println(f(5))\n",
                 "15\n");
    /* A for and a while nested in each other. */
    check_output("let i = 0\nwhile i < 2 {\n for j in 0..2 { print(i, j) }\n i += 1\n}\nprintln()\n",
                 "0 00 11 01 1\n");
    /* Shadowing an outer name. */
    check_output("let i = 100\nfor i in 0..2 { print(i) }\nprintln(i)\n", "01100\n");
}

static void test_fuel(void) {
    check_runs_out_of_fuel("for ;; {\n}\n", 500);
    check_runs_out_of_fuel("for let i = 0; true; {\n continue\n}\n", 500);
    check_runs_out_of_fuel("for i in 0..1000000000 { }\n", 500);
    check_runs_out_of_fuel("for let i = 0; i < 10; i += 1 {\n for j in 0..10 { }\n}\n", 50);
}

static void test_compile_errors(void) {
    check_compile_error("for x 1 {\n}\n",
                        "expected 'in' after the loop variable (a C-style loop starts with 'let': "
                        "for let i = 0; ...)",
                        1, 7);
    check_compile_error("for 5 in xs {\n}\n", "expected a name for the loop variable", 1, 5);
    check_compile_error("for x, 5 in xs {\n}\n", "expected a name for the loop variable", 1, 8);
    check_compile_error("for x in {\n}\n", "expected an expression after 'in'", 1, 10);
    check_compile_error("for x in [1] print(x)\n", "expected '{' after the 'for' header", 1, 14);
    check_compile_error("for i in 0..10 print(i)\n", "expected '{' after the 'for' header", 1, 16);
    check_compile_error("let r = 0..3\n", "a range can only be used in a 'for' loop", 1, 10);
    check_compile_error("println(0..3)\n", "a range can only be used in a 'for' loop", 1, 10);
    check_compile_error("for i in 0..10 by 0 {\n}\n", "range step cannot be 0", 1, 19);
    check_compile_error("for x in 5 {\n}\n", "cannot iterate over int", 1, 10);
    check_compile_error("for x in true {\n}\n", "cannot iterate over bool", 1, 10);
    check_compile_error("for x in nil {\n}\n", "cannot iterate over nil", 1, 10);
    check_compile_error("for x in [1, 2] {\n x = 5\n}\n", "cannot assign to the loop variable 'x'",
                        2, 2);
    check_compile_error("for x in [1, 2] {\n x += 5\n}\n",
                        "cannot assign to the loop variable 'x'", 2, 2);
    check_compile_error("for i, x in [1] {\n i = 0\n}\n",
                        "cannot assign to the loop variable 'i'", 2, 2);
    check_compile_error("for i, x in [1] {\n x = 0\n}\n",
                        "cannot assign to the loop variable 'x'", 2, 2);
    check_compile_error("for i in 0..3 {\n i = 0\n}\n", "cannot assign to the loop variable 'i'", 2,
                        2);
    check_compile_error("for i in \"a\"..3 {\n}\n", "range bounds must be ints, got string", 1, 10);
    check_compile_error("for i in 0..\"a\" {\n}\n", "range bounds must be ints, got string", 1, 13);
    check_compile_error("for i in 0..3 by \"a\" {\n}\n", "range bounds must be ints, got string", 1,
                        18);
    check_compile_error("for x: int in [\"a\"] {\n}\n", "type mismatch: expected int, got string", 1,
                        6);
    check_compile_error("for x: string in [1] {\n}\n", "type mismatch: expected string, got int", 1,
                        6);
    check_compile_error("for let i = 0 { }\n", "expected ';' after the loop initializer", 1, 15);
    check_compile_error("for let i = 0; i < 3\n{}\n", "expected ';' after the loop condition", 1, 21);
    check_compile_error("for i = 0; i < 3; i += 1 {\n}\n",
                        "expected 'in' after the loop variable (a C-style loop starts with 'let': "
                        "for let i = 0; ...)",
                        1, 7);
    check_compile_error("for let i = 0; i < 3; i < 3 {\n}\n",
                        "expected an assignment or a call in the loop step", 1, 23);
    check_compile_error("for let i = 0; i < 3; 5 {\n}\n",
                        "expected an assignment or a call in the loop step", 1, 23);
    check_run_error("for let i = 0; i < 3; i += 1 { }\nprintln(i)\n", "undefined variable 'i'");
}

static void test_run_errors(void) {
    /* A step that is only known at run time: the check moves to the loop. */
    check_run_error("let s = 0\nfor i in 0..3 by s {\n}\n", "range step cannot be 0");
    /* `any` hides the kind from the compiler. */
    check_run_error("let x: any = 5\nfor y in x {\n}\n", "cannot iterate over int");
    check_run_error("let x: any = nil\nfor y in x {\n}\n", "cannot iterate over nil");
    check_run_error("let a: any = \"s\"\nfor i in a..3 {\n}\n", "range bounds must be ints, got string");
    /* A static type that is `any` is still checked when the loop runs. */
    check_output("let b: any = 3\nfor i in 0..b { print(i) }\nprintln()\n", "012\n");
    check_run_error("let b: any = \"x\"\nfor i in 0..b {\n}\n",
                    "range bounds must be ints, got string");
    check_run_error("let s: any = \"x\"\nfor i in 0..3 by s {\n}\n",
                    "range bounds must be ints, got string");
    /* An `any` holding an array iterates like the array itself. */
    check_output("let x: any = [1, 2]\nfor y in x { print(y) }\nprintln()\n", "12\n");
}

static void test_names(void) {
    /* `in`, `by` and `for` stay usable as plain names, like the other keywords. */
    check_output("let a = 1\nprintln(a)\n", "1\n");
    check_output("for x in [1, 2] { print(x) }\nprintln()\n", "12\n");
    check_compile_error("let for = 1\n", "expected variable name after 'let'", 1, 5);
    check_compile_error("let in = 1\n", "expected variable name after 'let'", 1, 5);
}

int main(void) {
    test_arrays();
    test_strings();
    test_ranges();
    test_c_style();
    test_nested_and_scoped();
    test_fuel();
    test_compile_errors();
    test_run_errors();
    test_names();
    return LZ_TEST_RESULT();
}
