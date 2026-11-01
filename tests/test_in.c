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

/* A run-time failure with this message, reported at this line and column. */
static void check_run_error_at(const char *text, const char *message, uint32_t line,
                               uint32_t column) {
    Program program;
    run(&program, text, 0);
    LZ_CHECK(program.compile_status == LZ_OK);
    bool ok = program.run_status != LZ_OK && strstr(program.vm.error, message) != NULL;
    if (!ok) {
        fprintf(stderr, "program:\n%s\nexpected a run-time error with [%s]; got %d [%s]\n", text,
                message, (int)program.run_status, program.vm.error);
    }
    LZ_CHECK(ok);
    if (ok && program.vm.error_offset != LZ_NO_OFFSET) {
        LzLocation at = lz_source_locate(&program.source, program.vm.error_offset);
        if (at.line != line || at.column != column) {
            fprintf(stderr, "error at %u:%u, expected %u:%u\n", (unsigned)at.line,
                    (unsigned)at.column, (unsigned)line, (unsigned)column);
        }
        LZ_CHECK(at.line == line && at.column == column);
    } else {
        LZ_CHECK(ok && program.vm.error_offset != LZ_NO_OFFSET);
    }
    finish(&program);
}

static void test_arrays(void) {
    check_output("println(3 in [1, 2, 3], 4 in [1, 2, 3], 1 in [])\n", "true false false\n");
    check_output("println([1, 2] in [[0], [1, 2]], [3] in [[1, 2]])\n", "true false\n");
    check_output("println({\"a\": 1} in [{\"a\": 1}], {\"a\": 2} in [{\"a\": 1}])\n", "true false\n");
    check_output("println(\"b\" in [\"a\", \"b\"], \"c\" in [\"a\"], nil in [nil], nil in [1])\n",
                 "true false true false\n");
    check_output("println(1.0 in [1], 1.5 in [1, 2], 2 in [1.0, 2.0])\n", "true false true\n");
    check_output("println(3 not in [1, 2, 3], 4 not in [1, 2, 3], 1 not in [])\n",
                 "false true true\n");
}

static void test_dicts(void) {
    check_output("let d = {\"ana\": 1, 2: \"x\", true: 0}\n"
                 "println(\"ana\" in d, \"budi\" in d, 2 in d, 3 in d, true in d, false in d)\n",
                 "true false true false true false\n");
    /* Keys only, never values; a value that cannot be a key is simply absent. */
    check_output("let d = {\"a\": 1}\nprintln(1 in d, 1.5 in d, nil in d, [1] in d, {\"a\": 1} in d)\n",
                 "false false false false false\n");
    check_output("let d = {}\nprintln(\"a\" in d, \"a\" not in d)\n", "false true\n");
    check_output("let d = {\"x\": 1}\nprintln(\"x\" not in d, \"y\" not in d)\n", "false true\n");
}

static void test_strings(void) {
    check_output("println(\"ab\" in \"cabe\", \"zz\" in \"cabe\", \"\" in \"abc\", \"\" in \"\")\n",
                 "true false true true\n");
    check_output("println(\"ca\" in \"cabe\", \"be\" in \"cabe\", \"cabe!\" in \"cabe\")\n",
                 "true true false\n");
    check_output("println(\"é\" in \"café\", \"fé\" in \"café\", \"e\" in \"café\")\n",
                 "true true false\n");
    check_output("println(\"ab\" not in \"cabe\", \"zz\" not in \"cabe\")\n", "false true\n");
    check_compile_error("println(1 in \"abc\")\n",
                        "the left side of 'in' must be a string when the right side is a string, "
                        "got int", 1, 14);
    check_run_error_at("fn f(a, b) { return a in b }\nprintln(f(1, \"abc\"))\n",
                       "the left side of 'in' must be a string when the right side is a string, "
                       "got int", 1, 26);
    check_run_error("fn f(a, b) { return a in b }\nprintln(f(nil, \"abc\"))\n", "got nil");
}

static void test_other_right_sides(void) {
    check_compile_error("println(1 in 5)\n", "cannot use 'in' with int", 1, 14);
    check_compile_error("println(1 in true)\n", "cannot use 'in' with bool", 1, 14);
    check_compile_error("println(1 in nil)\n", "cannot use 'in' with nil", 1, 14);
    check_compile_error("println(1 not in 2.5)\n", "cannot use 'in' with float", 1, 18);
    /* Not known until run time: the same message, at the right operand. */
    check_run_error_at("fn f(a, b) { return a in b }\nprintln(f(1, 5))\n",
                       "cannot use 'in' with int", 1, 26);
    check_run_error("fn f(a, b) { return a not in b }\nprintln(f(1, nil))\n",
                    "cannot use 'in' with nil");
    check_run_error("println([[1], 2].map(fn(p) => 1 in p))\n", "cannot use 'in' with int");
    check_compile_error("println(1 in )\n", "expected an expression after 'in'", 1, 14);
}

static void test_ranges(void) {
    check_output("println(5 in 0..5, 4 in 0..5, 0 in 0..5, -1 in 0..5)\n", "false true true false\n");
    check_output("println(5 in 0..=5, 6 in 0..=5, 0 in 0..=5)\n", "true false true\n");
    check_output("println(5 in 5..5, 5 in 5..=5, 5 in 6..2)\n", "false true false\n");
    check_output("let age = 30\nprintln(age in 18..=65, age not in 18..=65, 70 not in 18..=65)\n",
                 "true false true\n");
    check_output("println(2.5 in 1..3, 3.0 in 1..3, 3.0 in 1..=3, 0.5 in 1..3)\n",
                 "true false true false\n");
    check_output("let xs = [1, 2, 3]\nlet n = 2\nprintln(n in 0..len(xs), 3 in 0..len(xs), n in 1 + 1..2 * 2)\n",
                 "true false true\n");
    check_output("println(9223372036854775807 in 0..=9223372036854775807)\n"
                 "println(9223372036854775807 in 0..9223372036854775807)\n"
                 "println(-9223372036854775807 - 1 in -9223372036854775807 - 1..0)\n",
                 "true\nfalse\ntrue\n");
    /* x, a and b are each evaluated once, left to right. */
    check_output("let log = []\n"
                 "fn t(n) { push(log, n)\n return n }\n"
                 "println(t(5) in t(1)..t(9))\nprintln(log)\n",
                 "true\n[5, 1, 9]\n");
    check_output("let c = 0\nfn t(n) { c += 1\n return n }\n"
                 "println(t(3) not in t(0)..=t(2), c)\n", "true 3\n");
    check_output("println(2 in 0..\n 5)\n", "true\n");
    check_compile_error("println(1 in 0..3 by 2)\n", "a range with 'by' cannot be used with 'in'", 1, 19);
    check_compile_error("println(1 not in 0..=3 by 2)\n",
                        "a range with 'by' cannot be used with 'in'", 1, 24);
    check_compile_error("println(\"a\" in 0..3)\n",
                        "the value tested with a range must be a number, got string", 1, 9);
    check_compile_error("println(nil in 0..3)\n",
                        "the value tested with a range must be a number, got nil", 1, 9);
    check_compile_error("println(1 in 0.5..3)\n", "range bounds must be ints, got float", 1, 14);
    check_compile_error("println(1 in 0..\"x\")\n", "range bounds must be ints, got string", 1, 17);
    check_run_error_at("fn f(x, a, b) { return x in a..b }\nprintln(f(\"s\", 1, 2))\n",
                       "the value tested with a range must be a number, got string", 1, 30);
    check_run_error("fn f(x, a, b) { return x in a..b }\nprintln(f(1, 1.5, 2))\n",
                    "range bounds must be ints, got float");
    check_run_error("fn f(x, a, b) { return x in a..b }\nprintln(f(1, 1, nil))\n",
                    "range bounds must be ints, got nil");
    /* A range is still not a value. */
    check_compile_error("let r = 0..3\n", "a range can only be used in a 'for' loop or after 'in'", 1, 10);
    check_compile_error("println(0..=3)\n", "a range can only be used in a 'for' loop or after 'in'", 1, 10);
    check_compile_error("println(1 in (0..3))\n",
                        "a range can only be used in a 'for' loop or after 'in'", 1, 16);
}

static void test_precedence(void) {
    check_output("let xs = [1]\nlet ys = [2]\nprintln(1 in xs and 2 in ys, 1 in xs and 3 in ys)\n",
                 "true false\n");
    check_output("let xs = [1]\nprintln(not (1 in xs), not 1 in xs)\n", "false false\n");
    check_output("let xs = [1]\nprintln(1 in xs == true, 2 in xs == false)\n", "true true\n");
    check_output("let xs = [3]\nprintln(1 + 2 in xs, 2 * 2 in xs)\n", "true false\n");
    check_output("let xs = [1]\nprintln(2 in xs or 1 in xs, 2 in xs or false)\n", "true false\n");
    /* Left associative: (1 in [1]) in [true]. */
    check_output("println(1 in [1] in [true], 2 in [1] in [true])\n", "true false\n");
    check_output("println(1 < 2 in [true], 1 in [1] == 2 in [2])\n", "true true\n");
}

static void test_for_headers(void) {
    check_output("for x in [1, 2] { print(x) }\nprintln()\n", "12\n");
    check_output("let d = {\"a\": 1}\nfor k, v in d { println(k, v) }\n", "a 1\n");
    check_output("for i in 0..5 by 2 { print(i) }\nprintln()\n", "024\n");
    check_output("for i in 0..=3 { print(i) }\nprintln()\n", "0123\n");
    check_output("for ch in \"hé\" { print(ch, \"|\") }\nprintln()\n", "h |é |\n");
    check_output("let xs = [0, 1, 2]\nfor let i = 0; i in xs; i += 1 { print(i) }\nprintln()\n", "012\n");
    check_output("let xs = [0, 1, 2]\nfor let i = 0; i not in [3]; i += 1 { if i > 1 { break }\n print(i) }\nprintln()\n",
                 "01\n");
    check_output("let xs = [1, 2]\nfor b in [1 in xs, 5 in xs] { print(b) }\nprintln()\n", "truefalse\n");
    check_output("fn f(b) { return [b] }\nlet xs = [1]\nfor b in f(1 in xs) { print(b) }\nprintln()\n", "true\n");
    check_output("let xs = [1]\nfor b in [(1 in xs)] { print(b) }\nprintln()\n", "true\n");
    check_output("let xs = [1]\nfor b in (xs).map(fn(x) => x in xs) { print(b) }\nprintln()\n", "true\n");
    check_output("let xs = [1]\nfor b in $\"{1 in xs}\" { print(b, \".\") }\nprintln()\n", "t .r .u .e .\n");
    /* At the top level of the header, the second `in` is not an operator. */
    check_compile_error("let xs = [1]\nfor x in xs in xs { }\n", "expected '{' after the 'for' header", 2, 13);
    check_compile_error("let xs = [1]\nfor x in xs not in xs { }\n", "expected '{' after the 'for' header", 2, 13);
}

static void test_fuel(void) {
    check_runs_out_of_fuel("let xs = [0]\nfor i in 0..14 { xs = xs + xs }\n"
                           "for i in 0..100 { -1 in xs }\n", 100000);
    check_runs_out_of_fuel("let s = \"ab\"\nfor i in 0..14 { s = s + s }\n"
                           "for i in 0..400 { \"zz\" in s }\n", 100000);
    check_output("let xs = [0]\nfor i in 0..14 { xs = xs + xs }\nprintln(-1 in xs, 0 in xs)\n", "false true\n");
}

static void test_contexts(void) {
    check_output("let xs = [1, 2]\nfn f(b) { return b }\nprintln(f(2 in xs))\n", "true\n");
    check_output("let xs = [1, 2]\nif 2 in xs { println(\"yes\") }\nif 3 not in xs { println(\"no\") }\n", "yes\nno\n");
    check_output("let xs = [1, 2]\nlet i = 0\nwhile i not in [3] { i += 1 }\nprintln(i)\n", "3\n");
    check_output("let xs = [1, 2]\nlet r = if 2 in xs { \"a\" } else { \"b\" }\nprintln(r)\n", "a\n");
    check_output("let xs = [1, 2]\nprintln($\"{2 in xs} {5 in xs}\")\n", "true false\n");
    check_output("let f = fn(x, xs) => x in xs\nprintln(f(1, [1]), f(2, [1]))\n", "true false\n");
    check_output("let xs = [1, 2]\nprintln([1, 2, 3, 4].filter(fn(x) => x in xs))\n", "[1, 2]\n");
    check_output("let xs = [1, 2]\nprintln([1, 5].filter(fn(x) => x not in xs))\n", "[5]\n");
    check_output("let a = [1, 2, 3]\nprintln(a.map(fn(x) => x in 2..=3))\n", "[false, true, true]\n");
}

static void test_coalesce(void) {
    check_output("println(nil ?? 1, 2 ?? 1)\n", "1 2\n");
    check_output("println(0 ?? 5, \"\" ?? \"x\", false ?? true, [] ?? [1])\n", "0  false []\n");
    check_output("let n = nil ?? nil ?? 0\nprintln(n)\n", "0\n");
    check_output("let a = nil\nlet b = 7\nprintln(a ?? b ?? 0, a ?? a ?? 9)\n", "7 9\n");
    /* The right side runs only when the left is nil. */
    check_output("let c = 0\nfn f() { c += 1\n return 1 }\nprintln(5 ?? f(), c)\nprintln(nil ?? f(), c)\n",
                 "5 0\n1 1\n");
    check_output("let c = 0\nfn f() { c += 1\n return nil }\nprintln(f() ?? f() ?? 3, c)\n", "3 2\n");
    /* Lowest of the binary operators. */
    check_output("println(nil or nil ?? 3, false or false ?? 3)\n", "3 false\n");
    check_output("println(nil ?? false or true, nil ?? 1 == 1, nil ?? true and false)\n",
                 "true true false\n");
    check_output("let d = {\"a\": 1}\nprintln(d.get(\"a\") ?? \"anon\", d.get(\"z\") ?? \"anon\")\n", "1 anon\n");
    check_output("let d = {\"a\": 1}\nprintln($\"{d.get(\"z\") ?? \"tidak ada\"} {d.get(\"a\") ?? 0}\")\n",
                 "tidak ada 1\n");
    check_output("let x = nil ??\n  4\nprintln(x)\n", "4\n");
    check_output("let x = nil\nlet y: int = x ?? 3\nprintln(y)\n", "3\n");
    check_output("let x: int? = nil\nlet y: int = x ?? 3\nprintln(y)\n", "3\n");
    check_output("let x: int? = 8\nlet y: int = x ?? 3\nprintln(y)\n", "8\n");
    check_output("let y: int | string = 5 ?? \"s\"\nprintln(y)\n", "5\n");
    check_compile_error("let x: int? = nil\nlet y: string = x ?? 3\n", "type mismatch", 2, 17);
    check_compile_error("let y = 1 ??\n", "expected an expression after '?" "?'", 2, 1);
    check_compile_error("let y = 1 ?? )\n", "expected an expression after '?" "?'", 1, 14);
    check_output("let f = fn(a, b) => a ?? b\nprintln(f(nil, 1), f(2, 1))\n", "1 2\n");
    check_output("println([nil, 1].map(fn(x) => x ?? 0))\n", "[0, 1]\n");
}

static void test_nullable_types(void) {
    check_output("let x: int? = nil\nprintln(x)\n", "nil\n");
    check_output("let xs: int?[] = [1, nil]\nprintln(xs)\n", "[1, nil]\n");
    check_output("let x: (int | string)? = nil\nprintln(x)\n", "nil\n");
    /* `int??` is nullable twice, which is nullable. */
    check_output("let x: int?? = nil\nprintln(x)\n", "nil\n");
    check_compile_error("let x: int? = \"s\"\n", "type mismatch", 1, 15);
    check_compile_error("let in = 1\n", "expected variable name after 'let'", 1, 5);
    check_compile_error("let not = 1\n", "expected variable name after 'let'", 1, 5);
    check_compile_error("let not in = 1\n", "expected variable name after 'let'", 1, 5);
}

int main(void) {
    test_arrays();
    test_dicts();
    test_strings();
    test_other_right_sides();
    test_ranges();
    test_precedence();
    test_for_headers();
    test_fuel();
    test_contexts();
    test_coalesce();
    test_nullable_types();
    return LZ_TEST_RESULT();
}
