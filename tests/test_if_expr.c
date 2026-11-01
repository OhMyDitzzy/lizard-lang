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
    LZ_CHECK(program->vm.stack_count == 0); /* every branch left exactly one value, and it was used */
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

/* The first error of the program, with the position of its caret. */
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

/* One mistake is one error: the try-again panic mode would not add a second here. */
static void check_one_error(const char *text, const char *message) {
    Program program;
    run(&program, text, 0);
    LZ_CHECK(program.compile_status == LZ_ERR_COMPILE);
    bool ok = program.diagnostics.count == 1 &&
              strstr(program.diagnostics.items[0].message, message) != NULL;
    if (!ok) {
        fprintf(stderr, "program:\n%s\nexpected exactly one error with [%s], got %zu\n", text,
                message, program.diagnostics.count);
        for (size_t i = 0; i < program.diagnostics.count; i++) {
            fprintf(stderr, "  %s\n", program.diagnostics.items[i].message);
        }
    }
    LZ_CHECK(ok);
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

static void test_values(void) {
    check_output("let n = 5\nprintln(if n > 0 { \"positif\" } else { \"non-positif\" })\n",
                 "positif\n");
    check_output("let n = -3\nprintln(if n > 0 { \"positif\" } else { \"non-positif\" })\n",
                 "non-positif\n");
    /* else-if chains, and the whole of it is what ends the statement. */
    check_output("let sign = fn(n) => if n > 0 { 1 } else if n < 0 { -1 } else { 0 }\n"
                 "println(sign(7), sign(-7), sign(0))\n",
                 "1 -1 0\n");
    /* Nested in a branch and in a condition. */
    check_output("let a = true\nlet b = false\n"
                 "println(if a { if b { 1 } else { 2 } } else { if b { 3 } else { 4 } })\n",
                 "2\n");
    check_output("let a = true\nprintln(if (if a { true } else { false }) { 1 } else { 2 })\n", "1\n");
    /* As an argument, an array item and an operand. */
    check_output("let a = true\nprintln(if a { 1 } else { 2 }, if a { \"y\" } else { \"n\" })\n",
                 "1 y\n");
    check_output("let a = true\nprintln([if a { 1 } else { 2 }, 3])\n", "[1, 3]\n");
    check_output("let a = true\nprintln((if a { 10 } else { 20 }) + 1)\n", "11\n");
    /* The operators after the last '}' apply to the whole expression. */
    check_output("let a = true\nprintln(if a { 2 } else { 3 } * 4)\n", "8\n");
    check_output("let a = true\nprintln(1 + if a { 2 } else { 3 })\n", "3\n");
    check_output("let a = true\nprintln(not if a { false } else { true })\n", "true\n");
    /* After 'return' and as the body of a lambda. */
    check_output("fn pick(ok: bool) -> string { return if ok { \"yes\" } else { \"no\" } }\n"
                 "println(pick(true), pick(false))\n",
                 "yes no\n");
    check_output("let abs = fn(x: int) -> int { return if x < 0 { -x } else { x } }\n"
                 "println(abs(-3), abs(4))\n",
                 "3 4\n");
    check_output("let f = fn(x) => if x < 0 { -x } else { x }\nprintln(f(-9))\n", "9\n");
    /* A lambda written inside a branch. */
    check_output("let a = true\nlet f = if a { fn(x) { return x * 2 } } else { fn(x) { return x } }\n"
                 "println(f(5))\n",
                 "10\n");
    /* Multi-line: a newline after '{' and before '}' is fine. */
    check_output("let a = true\nlet v = if a {\n    1\n} else {\n    2\n}\nprintln(v)\n", "1\n");
    /* A false condition of any falsy value takes the else. */
    check_output("println(if nil { 1 } else { 2 }, if 0 { 3 } else { 4 })\n", "2 3\n");
    check_output("println(if [] { 1 } else { 2 }, if \"\" { 3 } else { 4 })\n", "1 3\n");
}

static void test_short_circuit(void) {
    /* Only the branch taken runs: the other one is never evaluated. */
    check_output("let n = 0\nfn bump(v: int) -> int { n += 1\n return v }\n"
                 "println(if true { bump(1) } else { bump(2) }, n)\n",
                 "1 1\n");
    check_output("let n = 0\nfn bump(v: int) -> int { n += 1\n return v }\n"
                 "println(if false { bump(1) } else { bump(2) }, n)\n",
                 "2 1\n");
    /* An error in the branch not taken does not happen. */
    check_output("let xs = []\nprintln(if xs.len() > 0 { xs[0] } else { -1 })\n", "-1\n");
    check_output("let d = 0\nprintln(if d != 0 { 10 // d } else { 0 })\n", "0\n");
}

static void test_types(void) {
    /* The type of the result is the union of both branches. */
    check_output("let a = true\nlet v = if a { 1 } else { \"x\" }\nprintln(v)\n", "1\n");
    check_output("let a = true\nlet v: int | string = if a { 1 } else { \"x\" }\nprintln(v)\n", "1\n");
    check_output("let a = false\nlet v: int | string = if a { 1 } else { \"x\" }\nprintln(v)\n", "x\n");
    /* A narrower annotation is a run-time check, like any other assignment of a union. */
    check_output("let a = true\nlet v: int = if a { 1 } else { 2 }\nprintln(v + 1)\n", "2\n");
    check_run_error("let a = false\nlet v: int = if a { 1 } else { \"x\" }\nprintln(v)\n",
                    "type");
    /* Two branches of the same type give that type back. */
    check_output("fn half(x: int) -> float { let r: float = if x > 0 { x / 2 } else { 0.0 }\n"
                 " return r }\nprintln(half(4))\n",
                 "2.0\n");
    check_output("let xs = [if true { 1 } else { 2 }, if false { 3 } else { 4 }]\nprintln(xs)\n",
                 "[1, 4]\n");
}

static void test_templates(void) {
    check_output("let a = true\nprintln($\"{if a { 1 } else { 2 }}\")\n", "1\n");
    check_output("let a = false\nprintln($\"{if a { 1 } else { 2 }}\")\n", "2\n");
    /* A format spec applies to the value of the branch taken. */
    check_output("let a = true\nprintln($\"{if a { 42 } else { 7 }:03}\")\n", "042\n");
    check_output("let a = true\nprintln($\"x{if a { \"!\" } else { \"?\" }}\")\n", "x!\n");
    /* A string in a branch, and a template in a branch. */
    check_output("let a = true\nprintln($\"[{if a { \"yes\" } else { \"no\" }}]\")\n", "[yes]\n");
    check_output("let a = true\nprintln(if a { $\"{1 + 1}\" } else { \"no\" })\n", "2\n");
    /* A multi-line template. */
    check_output("let a = false\nprintln($\"\"\"\n  value {if a { \"yes\" } else { \"no\" }} here\n  \"\"\")\n",
                 "value no here\n");
    /* A nested if expression inside a template inside a branch. */
    check_output("let a = true\nprintln($\"{if a { $\"{if a { 1 } else { 2 }}\" } else { \"n\" }}\")\n",
                 "1\n");
}

static void test_statements_unchanged(void) {
    /* Statement form: block, else-if and else. */
    check_output("let n = 2\nif n == 1 {\n println(\"one\")\n} else if n == 2 {\n println(\"two\")\n}"
                 " else {\n println(\"many\")\n}\n",
                 "two\n");
    check_output("if true { println(\"a\") }\nprintln(\"b\")\n", "a\nb\n");
    /* break and continue inside an if statement in a loop. */
    check_output("for i in 0..10 {\n if i == 1 { continue }\n if i == 3 { break }\n println(i)\n}\n",
                 "0\n2\n");
    check_output("let i = 0\nwhile i < 5 {\n i += 1\n if i % 2 == 0 { continue }\n println(i)\n}\n",
                 "1\n3\n5\n");
    /* An if statement whose body uses an if expression. */
    check_output("let n = 3\nif n > 0 {\n let s = if n > 2 { \"big\" } else { \"small\" }\n"
                 " println(s)\n}\n",
                 "big\n");
    /* A branch holding a call of a function that returns an if expression. */
    check_output("fn f(x: int) -> int { return if x > 0 { 1 } else { 0 } }\n"
                 "if f(1) == 1 { println(\"ok\") }\n",
                 "ok\n");
}

static void test_stack_balance(void) {
    /* A branch's value is used every round: the stack must not grow. */
    check_output("let k = 0\nlet r = 0\nwhile k < 100000 { r += if k % 2 == 0 { 1 } else { 2 }\n"
                 " k += 1 }\nprintln(r)\n",
                 "150000\n");
    check_output("let r = 0\nfor i in 0..100000 { r += if i > 0 { 1 } else { 0 } }\nprintln(r)\n",
                 "99999\n");
    /* And a branch inside a nested loop. */
    check_output("let r = 0\nfor i in 0..100 {\n for j in 0..100 { r += if i == j { 1 } else { 0 } }\n}"
                 "println(r)\n",
                 "100\n");
}

static void test_errors(void) {
    check_compile_error("let x = if true { 1 }\n", "an 'if' expression needs an 'else'", 1, 9);
    check_compile_error("let x = if true { 1 } else if false { 2 }\n",
                        "an 'if' expression needs an 'else'", 1, 9);
    check_compile_error("let x = if true { let y = 1\n} else { 2 }\n",
                        "a branch of an 'if' expression holds one expression, not a statement", 1,
                        19);
    check_compile_error("let x = if true { 1 } else { while true { } }\n",
                        "a branch of an 'if' expression holds one expression, not a statement", 1,
                        30);
    check_compile_error("let x = if true { return 1 } else { 2 }\n",
                        "a branch of an 'if' expression holds one expression, not a statement", 1,
                        19);
    check_compile_error("let x = if true { } else { 2 }\n",
                        "expected an expression in the branch of an 'if' expression", 1, 19);
    check_compile_error("let x = if true { 1 } else { }\n",
                        "expected an expression in the branch of an 'if' expression", 1, 30);
    check_compile_error("let x = if true 1 else { 2 }\n", "expected '{' after the condition", 1, 17);
    check_compile_error("let x = if true { 1 2 } else { 2 }\n", "expected '}' after the expression", 1,
                        21);
    /* One mistake is one error: the rest of the expression still parses. */
    check_one_error("let x = if true { let y = 1\n} else { 2 }\nprintln(x)\n",
                    "a branch of an 'if' expression holds one expression");
    check_one_error("let x = if true { } else { 2 }\nprintln(x)\n",
                    "expected an expression in the branch");
    check_one_error("let x = if true { 1 2 } else { 2 }\nprintln(x)\n",
                    "expected '}' after the expression");
    /* A missing '{' does not leave the rest of the line to be read again. */
    check_one_error("let x = if true 1 else { 2 }\nprintln(x)\n", "expected '{' after the condition");
    /* An 'if' in a branch is an expression there too. */
    check_output("let b = false\nlet v = if true { if b { 1 } else { 2 } } else { 3 }\nprintln(v)\n",
                 "2\n");
    check_compile_error("println(if true { 1 })\n", "an 'if' expression needs an 'else'", 1, 9);
}

int main(void) {
    test_values();
    test_short_circuit();
    test_types();
    test_templates();
    test_statements_unchanged();
    test_stack_balance();
    test_errors();
    return LZ_TEST_RESULT();
}
