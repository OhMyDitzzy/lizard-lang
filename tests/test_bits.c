#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "lizard/array.h"
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

/* Fuel and heap limit of 0 mean unlimited. */
static void run(Program *program, const char *text, uint64_t fuel, size_t heap_limit) {
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
    if (heap_limit != 0) {
        lz_vm_set_heap_limit(&program->vm, heap_limit);
    }
    if (program->compile_status == LZ_OK) {
        program->run_status = lz_vm_run(&program->vm, &program->chunk);
    }
}

static void finish(Program *program) {
    LZ_CHECK(program->vm.stack_count == 0);
    lz_chunk_free(&program->vm.heap, &program->chunk);
    lz_vm_release_roots(&program->vm);
    LZ_CHECK(program->vm.heap.live_objects == 0); /* no leaked array, however it ended */
    LZ_CHECK(program->vm.heap.live_bytes == 0);   /* item buffers are accounted for and returned */
    lz_vm_free(&program->vm);
    lz_buffer_free(&program->out);
    lz_buffer_free(&program->err);
}

static void check_output(const char *text, const char *expected) {
    Program program;
    run(&program, text, 0, 0);
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

static void check_fails(const char *text, LzStatus status, const char *message, uint32_t line) {
    Program program;
    run(&program, text, 0, 0);
    LZ_CHECK(program.compile_status == LZ_OK);
    bool ok = program.run_status == status && strstr(program.vm.error, message) != NULL;
    if (!ok) {
        fprintf(stderr, "program:\n%s\nexpected status %d with [%s], got %d [%s]\n", text,
                (int)status, message, (int)program.run_status, program.vm.error);
    }
    LZ_CHECK(ok);
    if (line != 0) {
        LZ_CHECK(program.vm.error_offset != LZ_NO_OFFSET);
        LzLocation at = lz_source_locate(&program.source, program.vm.error_offset);
        if (at.line != line) {
            fprintf(stderr, "error at line %u, expected %u\n", (unsigned)at.line, (unsigned)line);
        }
        LZ_CHECK(at.line == line);
    }
    finish(&program);
}


static void check_compile_error(const char *text, const char *message, uint32_t line) {
    Program program;
    run(&program, text, 0, 0);
    LZ_CHECK(program.compile_status == LZ_ERR_COMPILE);
    bool found = false;
    for (size_t i = 0; i < program.diagnostics.count; i++) {
        const LzDiagnostic *d = &program.diagnostics.items[i];
        if (strstr(d->message, message) != NULL) {
            found = true;
            LZ_CHECK(lz_source_locate(&program.source, d->offset).line == line);
        }
    }
    if (!found) {
        fprintf(stderr, "program:\n%s\nexpected compile error [%s]\n", text, message);
        for (size_t i = 0; i < program.diagnostics.count; i++) {
            fprintf(stderr, "  got: %s\n", program.diagnostics.items[i].message);
        }
    }
    LZ_CHECK(found);
    finish(&program);
}

static void test_truth_tables(void) {
    check_output("println(0 & 0, 0 & 1, 1 & 0, 1 & 1)\n", "0 0 0 1\n");
    check_output("println(0 | 0, 0 | 1, 1 | 0, 1 | 1)\n", "0 1 1 1\n");
    check_output("println(0 ^ 0, 0 ^ 1, 1 ^ 0, 1 ^ 1)\n", "0 1 1 0\n");
    check_output("println(~0, ~1, ~-1, ~255)\n", "-1 -2 0 -256\n");
    check_output("println(-1 & 255, -2 | 1, -1 ^ 5, -8 & -3)\n", "255 -1 -6 -8\n");
    check_output("let mx = 9223372036854775807\nlet mn = -9223372036854775807 - 1\n"
                 "println(mx & mn, mx | mn, mx ^ mn, ~mx == mn, ~mn == mx)\n",
                 "0 -1 -1 true true\n");
    check_output("println(-9223372036854775808 | 0, ~-9223372036854775808)\n",
                 "-9223372036854775808 9223372036854775807\n");
    check_output("println(0xFF & 0x0F, 0b1010_0101 ^ 0xFF, 0b1010_0101 | 0o17, 0xF0 >> 4)\n",
                 "15 90 175 15\n");
}

static void test_precedence(void) {
    check_output("println(1 + 2 << 3, 1 << 2 + 1, 2 * 3 << 1, 16 >> 1 + 1)\n", "24 8 12 4\n");
    check_output("let flags = 5\nprintln(flags & 4 == 4, flags & 2 == 2, (flags & 4) == 4)\n",
                 "true false true\n");
    check_output("let a = 1\nlet b = 6\nlet c = 3\nprintln(a | b & c, (a | b) & c)\n", "3 3\n");
    check_output("println(1 | 6 & 3, 4 | 1 ^ 3, 5 ^ 3 | 8, 5 ^ 3 & 1)\n", "3 6 14 4\n");
    check_output("println(~1 + 1, -1 >> 1, ~1 << 1, - ~1, !1 == false)\n", "-1 -1 -4 2 true\n");
    check_output("println(1 | 2 < 4, 8 >> 1 == 4, 1 << 1 < 3)\n", "true true true\n");
    check_output("println(8 >> 1 >> 1, 1 << 1 << 2, 7 ^ 2 ^ 2, 6 - 3 - 1 << 1)\n", "2 8 7 4\n");
    check_output("println(1 |\n 2 &\n 3 <<\n 1)\n", "3\n");
    check_output("println(1 and 2 | 4, 0 or 3 & 1)\n", "6 0\n");
}

static void test_shifts(void) {
    check_output("let n = 63\nprintln(1 << 0, 1 << 63, 1 << n, -1 << 63, -8 >> 1, -1 >> 63, 5 >> 0)\n",
                 "1 -9223372036854775808 -9223372036854775808 -9223372036854775808 -4 -1 5\n");
    check_output("println(0x7FFFFFFFFFFFFFFF >> 62, 3 << 62, 0xFF << 60, -9223372036854775808 >> 63)\n",
                 "1 -4611686018427387904 -1152921504606846976 -1\n");
    check_fails("let n = 64\nprintln(1 << n)\n", LZ_ERR_RUNTIME, "shift count must be 0..63, got 64", 2);
    check_fails("let n = -1\nprintln(1 >> n)\n", LZ_ERR_RUNTIME, "shift count must be 0..63, got -1", 2);
    check_fails("let n = 100000\nprintln(1 << n)\n", LZ_ERR_RUNTIME, "got 100000", 2);
    check_fails("fn f(a, b) {\n    return a >> b\n}\nprintln(f(1, 64))\n", LZ_ERR_RUNTIME,
                "shift count must be 0..63, got 64", 2);
    check_compile_error("const k = 1 << 64\n", "shift count must be 0..63, got 64", 1);
    check_compile_error("const k = 1 >> -1\n", "shift count must be 0..63, got -1", 1);
    check_output("const k = 1 << 63\nconst m = 0xFF & 0x0F\nprintln(k, m, k >> 63)\n",
                 "-9223372036854775808 15 -1\n");
}

static void test_types(void) {
    check_compile_error("let x = 1.5 & 2\n", "operator '&' needs two ints, got float and int", 1);
    check_compile_error("let x = 1 | \"a\"\n", "operator '|' needs two ints, got int and string", 1);
    check_compile_error("let x = nil ^ 1\n", "operator '^' needs two ints, got nil and int", 1);
    check_compile_error("let x = [1] << 1\n", "operator '<<' needs two ints, got array and int", 1);
    check_compile_error("let x = 1 >> 2.0\n", "operator '>>' needs two ints, got int and float", 1);
    check_compile_error("let x = true & false\n", "(use 'and' for logic)", 1);
    check_compile_error("let x = true | false\n", "(use 'or' for logic)", 1);
    check_compile_error("let x = ~1.5\n", "operator '~' needs an int, got float", 1);
    check_compile_error("let x = ~\"s\"\n", "operator '~' needs an int, got string", 1);
    check_compile_error("let a = 1\nlet x = ~true\n", "operator '~' needs an int, got bool", 2);

    /* With no static type (any) the same message comes at run time, at the operator's line. */
    check_fails("let a = 1.5\nlet b = 2\nprintln(a & b)\n", LZ_ERR_RUNTIME,
                "operator '&' needs two ints, got float and int", 3);
    check_fails("let a = \"x\"\nprintln(1 | a)\n", LZ_ERR_RUNTIME,
                "operator '|' needs two ints, got int and string", 2);
    check_fails("let a = true\nlet b = false\nprintln(a & b)\n", LZ_ERR_RUNTIME,
                "operator '&' needs two ints, got bool and bool (use 'and' for logic)", 3);
    check_fails("let a = nil\nprintln(a ^ 1)\n", LZ_ERR_RUNTIME,
                "operator '^' needs two ints, got nil and int", 2);
    check_fails("let a = [1]\nprintln(a << 1)\n", LZ_ERR_RUNTIME,
                "operator '<<' needs two ints, got array and int", 2);
    check_fails("let a = 2.5\nprintln(~a)\n", LZ_ERR_RUNTIME, "operator '~' needs an int, got float", 2);
    check_fails("let a = nil\nprintln(1 >>\n    a)\n", LZ_ERR_RUNTIME,
                "operator '>>' needs two ints, got int and nil", 2);

    /* The result is an int, so it fits an int slot and is checked against narrower ones. */
    check_output("let x: int = 6 & 3\nlet y: int | string = x | 1\nprintln(x, y)\n", "2 3\n");
    check_output("let a = 3\nlet b = 5\nlet x: int | string = a | b\nprintln(x)\n", "7\n");
    check_compile_error("let x: string = 1 | 2\n", "type mismatch", 1);
}

static void test_other_operators_unaffected(void) {
    check_output("println(true && false, true || false, 1 and 2, nil or 5, 0 && 1, 0 || 3)\n",
                 "false true 2 5 1 0\n");
    check_output("fn f(x: int | string) {\n    return x\n}\nprintln(f(1), f(\"a\"))\n", "1 a\n");
    check_output("fn g(x: int | nil) => x\nprintln(g(nil), g(2))\n", "nil 2\n");
    check_compile_error("let x: int || string = 1\n", "needs an initial value", 1); /* '||' is not a type union */
    check_output("println(1 < 2, 2 <= 2, 3 > 4, 4 >= 5, 1 == 1, 1 != 1)\n",
                 "true true false false true false\n");
    check_output("let f = fn(x) => x << 1\nprintln(f(4))\n", "8\n");
    check_output("println($\"{255:#b} {5 << 2} {6 | 1:#06b} {~5} {-8 >> 1:>4}\")\n",
                 "0b11111111 20 0b0111 -6   -4\n");
    check_output("println(5 ^ 2)\n", "7\n"); /* XOR, not a power */
}

static void test_folding_matches_runtime(void) {
    /* The same expression as a constant (folded at compile time) and with variables. */
    static const char *const exprs[] = {
        "0xF0F0 & 0xFF00",  "0xF0F0 | 0x0F0F", "0xF0F0 ^ 0xFFFF", "~0xF0F0",
        "-1 >> 1",          "-8 >> 1",         "1 << 62",         "1 << 63",
        "0xFF << 56",       "-1 << 1",         "5 & -2 | 8 ^ 3",  "1 + 2 << 3",
        "~5 + 1 << 2",      "0b1010_0101 >> 2 & 0x7",
    };
    for (size_t i = 0; i < sizeof exprs / sizeof exprs[0]; i++) {
        Program folded;
        char text[256];
        snprintf(text, sizeof text, "const c = %s\nprintln(c)\n", exprs[i]);
        run(&folded, text, 0, 0);
        LZ_CHECK(folded.compile_status == LZ_OK && folded.run_status == LZ_OK);

        Program runtime;
        /* A function parameter keeps the operands from being literals only in the global scope. */
        snprintf(text, sizeof text, "fn f() {\n    let r = %s\n    return r\n}\nprintln(f())\n", exprs[i]);
        run(&runtime, text, 0, 0);
        LZ_CHECK(runtime.compile_status == LZ_OK && runtime.run_status == LZ_OK);
        const char *a = folded.out.data != NULL ? folded.out.data : "";
        const char *b = runtime.out.data != NULL ? runtime.out.data : "";
        if (strcmp(a, b) != 0) {
            fprintf(stderr, "[%s]: folded %s vs runtime %s\n", exprs[i], a, b);
        }
        LZ_CHECK(strcmp(a, b) == 0);
        finish(&folded);
        finish(&runtime);
    }
    check_output("const a = 12\nconst b = a & 10 | 1\nconst c = b << 2\nprintln(a, b, c)\n", "12 9 36\n");
    check_compile_error("const a = 1.5\nconst b = a & 1\n", "needs two ints", 2);
}

static void test_bit_methods(void) {
    check_output("println(0.bit_count(), 1.bit_count(), 255.bit_count(), (-1).bit_count(), (-8).bit_count())\n",
                 "0 1 8 64 61\n");
    check_output("let mn = -9223372036854775807 - 1\nlet mx = 9223372036854775807\n"
                 "println(mn.bit_count(), mx.bit_count(), mn.bit_length(), mx.bit_length())\n",
                 "1 63 64 63\n");
    check_output("println(0.bit_length(), 1.bit_length(), 255.bit_length(), 256.bit_length(), "
                 "(-1).bit_length(), (-256).bit_length(), (-255).bit_length())\n",
                 "0 1 8 9 1 9 8\n");
    check_output("println(5.bit(0), 5.bit(1), 5.bit(2), 5.bit(63), (-1).bit(63), (-1).bit(0), 0.bit(0))\n",
                 "true false true false true true false\n");
    check_output("let x = 0b1010\nprintln(x.bit(1), x.bit(2), (x << 3).bit(6))\n", "true false true\n");
    check_fails("println(5.bit(64))\n", LZ_ERR_RUNTIME, "bit index must be 0..63, got 64", 1);
    check_fails("println(5.bit(-1))\n", LZ_ERR_RUNTIME, "bit index must be 0..63, got -1", 1);
    check_fails("println(5.bit(\"a\"))\n", LZ_ERR_RUNTIME, "expected an int, got string", 1);
    check_fails("println(5.bit())\n", LZ_ERR_ARITY, "bit(): expected 1 argument, got 0", 1);
    check_fails("println(5.bit_count(1))\n", LZ_ERR_ARITY, "bit_count(): expected 0 arguments, got 1", 1);
    check_fails("println(2.5.bit_count())\n", LZ_ERR_RUNTIME, "float has no method 'bit_count' (it works on int)", 1);
    check_fails("println(\"a\".bit_length())\n", LZ_ERR_RUNTIME, "string has no method 'bit_length'", 1);
    check_fails("println(true.bit(0))\n", LZ_ERR_RUNTIME, "bool has no method 'bit'", 1);
    check_fails("println(nil.bit_count())\n", LZ_ERR_RUNTIME, "nil has no method 'bit_count'", 1);
}

int main(void) {
    test_truth_tables();
    test_precedence();
    test_shifts();
    test_types();
    test_other_operators_unaffected();
    test_folding_matches_runtime();
    test_bit_methods();
    return LZ_TEST_RESULT();
}
