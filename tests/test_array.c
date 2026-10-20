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

static void check_compile_error(const char *text, const char *message) {
    Program program;
    run(&program, text, 0, 0);
    LZ_CHECK(program.compile_status == LZ_ERR_COMPILE);
    bool found = false;
    for (size_t i = 0; i < program.diagnostics.count; i++) {
        found = found || strstr(program.diagnostics.items[i].message, message) != NULL;
    }
    if (!found) {
        fprintf(stderr, "program:\n%s\nexpected an error containing [%s], got %zu:\n", text,
                message, program.diagnostics.count);
        for (size_t i = 0; i < program.diagnostics.count; i++) {
            fprintf(stderr, "  %s\n", program.diagnostics.items[i].message);
        }
    }
    LZ_CHECK(found);
    finish(&program);
}

static void check_runtime_error(const char *text, LzStatus status, const char *message) {
    Program program;
    run(&program, text, 0, 0);
    LZ_CHECK(program.compile_status == LZ_OK);
    bool ok = program.run_status == status && strstr(program.vm.error, message) != NULL;
    if (!ok) {
        fprintf(stderr, "program:\n%s\nexpected status %d with [%s], got %d [%s]\n", text,
                (int)status, message, (int)program.run_status, program.vm.error);
    }
    LZ_CHECK(ok);
    finish(&program);
}

/* A nan: the language has no literal for it, but infinity minus infinity is one. */
#define NAN_PRELUDE "let big = 1e308\nlet inf = big * 10\nlet nan = inf - inf\n"

static void test_literals_and_printing(void) {
    check_output("let xs = [1, 2, 3]\nprintln(xs)\n", "[1, 2, 3]\n");
    check_output("println([])\nprintln([[]])\nprintln([[1], [2, 3]])\n",
                 "[]\n[[]]\n[[1], [2, 3]]\n");
    check_output("println([1, \"a\", 2.5, true, nil])\n", "[1, \"a\", 2.5, true, nil]\n");
    check_output("print([\"q\\\"uote\", \"tab\\t\", \"nl\\n\"])\nprintln()\n",
                 "[\"q\\\"uote\", \"tab\\t\", \"nl\\n\"]\n");
    check_output("println(string([1, \"a\"]), \"x\" + string([2]))\n", "[1, \"a\"] x[2]\n");
    check_output("let a = [\n  1,\n  2,\n]\nprintln(a, [1, 2,])\n", "[1, 2] [1, 2]\n");
    check_output("let n = 2\nprintln([n, n * 2, n + 1])\n", "[2, 4, 3]\n");
}

static void test_indexing(void) {
    check_output("let xs = [1, 2, 3]\nprintln(xs[0], xs[2], xs[-1], xs[-3])\n", "1 3 3 1\n");
    check_output("let xs = [1, 2, 3]\nxs[0] = 10\nxs[-1] = 30\nprintln(xs)\n", "[10, 2, 30]\n");
    check_output("let g = [[1, 2], [3, 4]]\ng[1][0] = 9\nprintln(g, g[1][0], g[0][-1])\n",
                 "[[1, 2], [9, 4]] 9 2\n");
    check_output("println([10, 20][1], [[1, 2]][0][1])\n", "20 2\n");
    check_output("let xs = [1, 2]\nlet i = 1\nxs[i] = xs[i - 1] + 5\nprintln(xs)\n", "[1, 6]\n");
    check_output("const xs = [1, 2]\nxs[0] = 7\nprintln(xs)\n", "[7, 2]\n");
    check_runtime_error("let xs = [1, 2, 3]\nprintln(xs[3])\n", LZ_ERR_RUNTIME,
                        "index 3 out of range for array of length 3");
    check_runtime_error("let xs = [1, 2, 3]\nprintln(xs[-4])\n", LZ_ERR_RUNTIME,
                        "index -4 out of range for array of length 3");
    check_runtime_error("println([][0])\n", LZ_ERR_RUNTIME, "out of range for array of length 0");
    check_runtime_error("let xs = [1]\nxs[1] = 2\n", LZ_ERR_RUNTIME, "out of range");
    check_runtime_error("let xs = [1]\nlet i = 0.0\nprintln(xs[i])\n", LZ_ERR_RUNTIME,
                        "array index must be an int, got float");
    check_runtime_error("let s = \"abc\"\nprintln(s[0])\n", LZ_ERR_RUNTIME,
                        "cannot index a value of type string");
    check_runtime_error("let n = nil\nn[0] = 1\n", LZ_ERR_RUNTIME,
                        "cannot index a value of type nil");
    check_compile_error("println(\"abc\"[0])\n", "cannot index a value of type");
    check_compile_error("let xs = [1]\nprintln(xs[\"a\"])\n", "an array index must be an int");
    check_compile_error("let xs = [1]\nxs[0 = 1\n", "expected ']' after index");
    check_compile_error("let xs: int[] = [1]\nxs[0] = \"s\"\n", "type mismatch");
}

static void test_slices(void) {
    check_output("let xs = [1, 2, 3, 4, 5]\nprintln(xs[1:3], xs[:2], xs[3:], xs[:])\n",
                 "[2, 3] [1, 2] [4, 5] [1, 2, 3, 4, 5]\n");
    check_output("let xs = [1, 2, 3, 4, 5]\nprintln(xs[-2:], xs[:-1], xs[-3:-1], xs[1:-1])\n",
                 "[4, 5] [1, 2, 3, 4] [3, 4] [2, 3, 4]\n");
    check_output("let xs = [1, 2, 3]\nprintln(xs[2:1], xs[5:], xs[:9], xs[-9:2], xs[3:3])\n",
                 "[] [] [1, 2, 3] [1, 2] []\n");
    check_output("let xs = [1, 2, 3]\nlet ys = xs[:]\nys[0] = 99\nprintln(xs, ys)\n",
                 "[1, 2, 3] [99, 2, 3]\n");
    check_output("let xs = [[1], [2]]\nlet ys = xs[:1]\nys[0][0] = 7\nprintln(xs)\n",
                 "[[7], [2]]\n"); /* shallow: the rows are shared */
    check_output("println([1, 2, 3, 4][1:3][0])\n", "2\n");
    check_runtime_error("let xs = [1]\nlet f = 0.5\nprintln(xs[f:])\n", LZ_ERR_RUNTIME,
                        "slice bounds must be ints");
    check_compile_error("let xs = [1]\nprintln(xs[0.5:])\n", "a slice bound must be an int");
    check_compile_error("let xs = [1]\nprintln(xs[\"a\":])\n", "a slice bound must be an int");
    check_compile_error("let xs = [1]\nprintln(xs[1:2)\n", "expected ']' after slice");
    check_compile_error("println(\"abc\"[1:2])\n", "cannot index a value of type");
    check_runtime_error("let s = \"abc\"\nprintln(s[1:2])\n", LZ_ERR_RUNTIME,
                        "cannot index a value of type string");
}

static void test_operators(void) {
    check_output("let a = [1, 2]\nlet b = [3]\nlet c = a + b\nprintln(c, a, b, a + [] , [] + [])\n",
                 "[1, 2, 3] [1, 2] [3] [1, 2] []\n");
    check_output("let a = [1]\nlet c = a + a\nc[0] = 5\nprintln(a, c)\n", "[1] [5, 1]\n");
    check_output("println([1, 2] == [1, 2], [1, 2] == [1, 3], [1] == [1, 2], [] == [])\n",
                 "true false false true\n");
    check_output("println([1, 2] != [1, 2], [1] == 1, 1 == [1], [1] == nil)\n",
                 "false false false false\n");
    check_output("println([1] == [1.0], [[1, [2]]] == [[1, [2]]], [[1, [2]]] == [[1, [3]]])\n",
                 "true true false\n");
    check_output("let a = [1]\nprintln(a == a, a == [1], a == a + [])\n", "true true true\n");
    check_output("println([\"a\"] == [\"a\"], [\"a\"] == [\"b\"])\n", "true false\n");
    /* Only nil and false are falsy, as everywhere else. */
    check_output("if [] { println(\"truthy\") }\nprintln(![], [] and 1, bool([]))\n",
                 "truthy\nfalse 1 true\n");
    check_runtime_error("let a = [1]\nlet n = 1\nprintln(a + n)\n", LZ_ERR_RUNTIME,
                        "two numbers or two strings");
    check_runtime_error("let a = [1]\nlet b = 2\nprintln(a - a)\n", LZ_ERR_RUNTIME,
                        "operands must be numbers");
    check_compile_error("println([1] - [2])\n", "operator '-' cannot be applied");
    check_compile_error("println([1] + 1)\n", "operator '+' cannot be applied");
    check_compile_error("println(-[1])\n", "operator '-' cannot be applied");
    check_compile_error("println([1] < [2])\n", "operator '<' cannot be applied");
}

static void test_builtins(void) {
    check_output(
        "println(len([]), len([1, 2, 3]), len(\"\"), len(\"abc\"), len(\"h\\u{e9}llo\"))\n",
        "0 3 0 3 5\n");
    check_output("let xs = [1]\npush(xs, 2)\npush(xs, [3])\nprintln(xs, len(xs))\n",
                 "[1, 2, [3]] 3\n");
    check_output("let xs = [1, 2, 3]\nprintln(pop(xs), pop(xs), xs, pop(xs), xs)\n",
                 "3 2 [] 1 []\n");
    check_output("let xs = [1, 2, 3]\ninsert(xs, 0, 0)\ninsert(xs, 4, 4)\ninsert(xs, -1, 9)\n"
                 "println(xs)\n",
                 "[0, 1, 2, 3, 9, 4]\n");
    check_output("let xs = [1, 2, 3, 4]\nprintln(remove(xs, 1), remove(xs, -1), xs)\n",
                 "2 4 [1, 3]\n");
    check_output("let xs = [1, 2]\nclear(xs)\nprintln(xs, len(xs))\npush(xs, 5)\nprintln(xs)\n",
                 "[] 0\n[5]\n");
    check_output("let a = [1, [2]]\nlet b = copy(a)\npush(b, 3)\nb[0] = 9\nb[1][0] = 8\n"
                 "println(a, b)\n",
                 "[1, [8]] [9, [8], 3]\n");
    check_output("let xs = [1, 2.0, \"a\", [1]]\nprintln(contains(xs, 2), contains(xs, \"a\"), "
                 "contains(xs, [1]), contains(xs, 5), contains([], 1))\n",
                 "true true true false false\n");
    check_output("let xs = [5, 6, 5]\nprintln(index_of(xs, 5), index_of(xs, 6), index_of(xs, 7), "
                 "index_of([[1], [2]], [2]))\n",
                 "0 1 -1 1\n");
    check_output("let xs = [1, 2, 3, 4]\nreverse(xs)\nprintln(xs)\nlet o = [1, 2, 3]\nreverse(o)\n"
                 "reverse([])\nprintln(o)\n",
                 "[4, 3, 2, 1]\n[3, 2, 1]\n");
    check_output(
        "let xs = [3, 1, 2]\nsort(xs)\nprintln(xs)\nlet f = [2.5, 1, -3, 1.5, 10]\nsort(f)\n"
        "println(f)\nlet s = [\"b\", \"abc\", \"a\", \"\", \"B\"]\nsort(s)\nprintln(s)\n",
        "[1, 2, 3]\n[-3, 1, 1.5, 2.5, 10]\n[\"\", \"B\", \"a\", \"abc\", \"b\"]\n");
    check_output("let e = []\nsort(e)\nlet one = [1]\nsort(one)\nprintln(e, one)\n", "[] [1]\n");
    check_output("println(join([1, 2, 3], \", \"), join([], \"-\"), join([\"a\"], \"-\"), "
                 "join([\"x\", 1, 2.5, true, nil, [1, \"b\"]], \"|\"))\n",
                 "1, 2, 3  a x|1|2.5|true|nil|[1, \"b\"]\n");
    check_output(
        "println(range(0), range(3), range(2, 5), range(5, 2), range(-2, 1), range(3, 3))\n",
        "[] [0, 1, 2] [2, 3, 4] [] [-2, -1, 0] []\n");
    check_output(
        "println(sum([]), sum([1, 2, 3]), sum([1, 2.5]), sum([0.5, 1]), sum(range(101)))\n",
        "0 6 3.5 1.5 5050\n");
    check_output("println(min([3, 1, 2]), max([3, 1, 2]), min([2.5, 1]), max([1, 2.5, 2]), "
                 "min([-1, -5]))\n",
                 "1 3 1 2.5 -5\n");
    check_output("println(min([1, 1.0]), max([1.0, 1]))\n", "1 1.0\n");
}

static void test_builtin_errors(void) {
    check_runtime_error("push(1, 2)\n", LZ_ERR_RUNTIME, "push(): expected an array, got int");
    check_runtime_error("println(len(5))\n", LZ_ERR_RUNTIME, "len(): expected an array or string");
    check_runtime_error("println(pop([]))\n", LZ_ERR_RUNTIME, "pop(): array is empty");
    check_runtime_error("insert([1], 2, 0)\n", LZ_ERR_RUNTIME, "insert(): index 2 out of range");
    check_runtime_error("insert([1], -3, 0)\n", LZ_ERR_RUNTIME, "insert(): index -3 out of range");
    check_runtime_error("insert([1], \"a\", 0)\n", LZ_ERR_RUNTIME, "insert(): expected an int");
    check_runtime_error("remove([1], 1)\n", LZ_ERR_RUNTIME, "remove(): index 1 out of range");
    check_runtime_error("remove([], 0)\n", LZ_ERR_RUNTIME, "remove(): index 0 out of range");
    check_runtime_error("sort([1, \"a\"])\n", LZ_ERR_RUNTIME,
                        "sort(): cannot sort mixed types (int and string)");
    check_runtime_error("sort([\"a\", 1])\n", LZ_ERR_RUNTIME,
                        "cannot sort mixed types (string and int)");
    check_runtime_error("sort([true, false])\n", LZ_ERR_RUNTIME, "cannot sort values of type bool");
    check_runtime_error("sort([[1], [0]])\n", LZ_ERR_RUNTIME, "cannot sort values of type array");
    check_runtime_error("sort([nil])\n", LZ_ERR_RUNTIME, "cannot sort values of type nil");
    check_runtime_error(NAN_PRELUDE "sort([1, nan, 2])\n", LZ_ERR_RUNTIME, "cannot sort nan");
    check_runtime_error("join([1], 5)\n", LZ_ERR_RUNTIME, "join(): separator must be a string");
    check_runtime_error("join(5, \",\")\n", LZ_ERR_RUNTIME, "join(): expected an array");
    check_runtime_error("range(\"a\")\n", LZ_ERR_RUNTIME, "range(): expected an int");
    check_runtime_error("range(1, 2.5)\n", LZ_ERR_RUNTIME, "range(): expected an int");
    check_runtime_error("range(0, 100000000)\n", LZ_ERR_RUNTIME, "range(): too many items");
    check_runtime_error("range(-9223372036854775807 - 1, 9223372036854775807)\n", LZ_ERR_RUNTIME,
                        "range(): too many items");
    check_runtime_error("sum([1, \"a\"])\n", LZ_ERR_RUNTIME,
                        "sum(): expected an array of numbers, got string at index 1");
    check_runtime_error("sum([9223372036854775807, 1])\n", LZ_ERR_RUNTIME,
                        "sum(): integer overflow");
    check_runtime_error("sum([-9223372036854775807 - 1, -1])\n", LZ_ERR_RUNTIME,
                        "integer overflow");
    check_runtime_error("min([])\n", LZ_ERR_RUNTIME, "min(): array is empty");
    check_runtime_error("max([])\n", LZ_ERR_RUNTIME, "max(): array is empty");
    check_runtime_error("max([1, nil])\n", LZ_ERR_RUNTIME, "max(): expected an array of numbers");
    check_runtime_error(NAN_PRELUDE "println(min([1, nan]))\n", LZ_ERR_RUNTIME,
                        "min(): cannot compare nan");
    check_runtime_error("push([])\n", LZ_ERR_ARITY, "push(): wrong number of arguments");
    check_runtime_error("range()\n", LZ_ERR_ARITY, "range(): wrong number of arguments");
    check_runtime_error("range(1, 2, 3)\n", LZ_ERR_ARITY, "range(): wrong number of arguments");
}

static void test_failed_calls_change_nothing(void) {
    /* A failed sort, push or insert leaves the array exactly as it was. */
    check_output("let xs = [3, \"a\", 1]\nlet ok = true\n"
                 "let kept = copy(xs)\nprintln(kept)\n",
                 "[3, \"a\", 1]\n");
    check_runtime_error("let xs = [3, 1, \"a\"]\nsort(xs)\n", LZ_ERR_RUNTIME, "mixed types");
}

static void test_typed_arrays(void) {
    check_output("let ys: int[] = [4, 5]\nlet k: string[] = []\nprintln(ys, k)\n", "[4, 5] []\n");
    check_output("let ys: int[] = [4, 5]\npush(ys, 6)\nys[0] = 1\ninsert(ys, 0, 0)\nprintln(ys)\n",
                 "[0, 1, 5, 6]\n");
    check_runtime_error("let ys: int[] = [4]\npush(ys, \"x\")\n", LZ_ERR_TYPE,
                        "type error: array of int cannot hold string");
    check_runtime_error("let k: string[] = []\npush(k, 5)\n", LZ_ERR_TYPE,
                        "array of string cannot hold int");
    check_runtime_error("let ys: int[] = [4]\nlet v = 1.5\nys[0] = v\n", LZ_ERR_TYPE,
                        "array of int cannot hold float");
    check_runtime_error("let ys: int[] = [4]\nlet v = nil\nys[0] = v\n", LZ_ERR_TYPE,
                        "cannot hold nil");
    check_compile_error("let ys: int[] = [4]\nys[0] = 1.5\n",
                        "type mismatch: expected int, got float");
    check_runtime_error("let ys: int[] = [4]\ninsert(ys, 0, \"x\")\n", LZ_ERR_TYPE,
                        "cannot hold string");
    check_runtime_error("let ys: int[] = [1]\nlet z = [[1]]\nlet w: int[] = z\n", LZ_ERR_TYPE,
                        "expected int[], got");
    check_compile_error("let a: float[] = [1.5]\nlet b: int[] = a\n",
                        "type mismatch: expected int[], got float[]");
    check_output("let n: int[]? = nil\nlet m: int[]? = [1]\nprintln(n, m)\n", "nil [1]\n");
    check_output("let u: (int | string)[] = [1, \"a\"]\npush(u, 2)\npush(u, \"b\")\nprintln(u)\n",
                 "[1, \"a\", 2, \"b\"]\n");
    check_runtime_error("let u: (int | string)[] = [1]\npush(u, 1.5)\n", LZ_ERR_TYPE,
                        "array of int | string cannot hold float");
    check_output("let m: int?[] = [1, nil]\npush(m, nil)\npush(m, 3)\nprintln(m)\n",
                 "[1, nil, nil, 3]\n");
    check_output(
        "type Mode = \"r\" | \"w\"\nlet ms: Mode[] = [\"r\"]\npush(ms, \"w\")\nprintln(ms)\n",
        "[\"r\", \"w\"]\n");
    check_runtime_error("type Mode = \"r\" | \"w\"\nlet ms: Mode[] = [\"r\"]\npush(ms, \"x\")\n",
                        LZ_ERR_TYPE, "cannot hold string");
}

static void test_typed_assignment_checks(void) {
    /* An untyped value is checked when it is stored under a typed slot, and typed from then on. */
    check_output("let a = [1, 2]\nlet b: int[] = a\npush(a, 3)\nprintln(a, b)\n",
                 "[1, 2, 3] [1, 2, 3]\n");
    check_runtime_error("let a = [1, 2]\nlet b: int[] = a\npush(a, \"x\")\n", LZ_ERR_TYPE,
                        "cannot hold string");
    check_runtime_error("let a = [1, \"x\"]\nlet b: int[] = a\n", LZ_ERR_TYPE, "expected int[]");
    check_output("let b: int[] = [1]\nb = [2, 3]\nprintln(b)\n", "[2, 3]\n");
    check_compile_error("let b: int[] = [1]\nb = [\"s\"]\n",
                        "type mismatch: expected int[], got string[]");
    check_runtime_error("let b: int[] = [1]\nlet v = [\"s\"]\nb = v\n", LZ_ERR_TYPE,
                        "expected int[], got any[]");
    check_compile_error("let b: int[] = [\"s\"]\n", "type mismatch: expected int[], got string[]");
    check_compile_error("let b: int[] = 5\n", "type mismatch");
    check_compile_error("let b: string[] = [1, 2]\n", "type mismatch");
    check_compile_error("let b: int = [1]\n", "type mismatch: expected int, got int[]");
    check_output("let b: int[] = []\nlet c: string[] = []\nprintln(b, c)\n", "[] []\n");
    /* Nested typing: rows stay typed. */
    check_output(
        "let g: int[][] = [[1], [2]]\ng[0][0] = 5\npush(g[1], 6)\npush(g, [7])\nprintln(g)\n",
        "[[5], [2, 6], [7]]\n");
    check_runtime_error("let g: int[][] = [[1]]\npush(g[0], \"x\")\n", LZ_ERR_TYPE,
                        "cannot hold string");
    check_runtime_error("let g: int[][] = [[1]]\npush(g, [\"x\"])\n", LZ_ERR_TYPE, "cannot hold");
    check_runtime_error("let g: int[][] = [[1]]\npush(g, 5)\n", LZ_ERR_TYPE, "cannot hold int");
    check_runtime_error("let g: int[][] = [[1]]\nlet row = [2]\npush(g, row)\npush(row, \"x\")\n",
                        LZ_ERR_TYPE, "cannot hold string");
    check_output("let g: int[][][] = [[[1]]]\npush(g[0][0], 2)\nprintln(g)\n", "[[[1, 2]]]\n");
    check_runtime_error("let g: int[][][] = [[[1]]]\npush(g[0][0], \"x\")\n", LZ_ERR_TYPE,
                        "cannot hold");
    /* The same array cannot be two unrelated types. */
    check_compile_error("let a: int[] = []\nlet b: string[] = a\n",
                        "type mismatch: expected string[], got int[]");
    check_runtime_error("let a: int[] = []\nlet c = a\nlet b: string[] = c\n", LZ_ERR_TYPE,
                        "expected string[], got int[]");
    /* ...but may be viewed as a wider one. */
    check_output("let a: int[] = [1]\nlet b: (int | string)[] = a\nprintln(b)\n", "[1]\n");
    /* Concat keeps the type only when both sides agree. */
    check_output("let a: int[] = [1]\nlet b: int[] = [2]\nlet c = a + b\npush(c, 3)\nprintln(c)\n",
                 "[1, 2, 3]\n");
    check_runtime_error(
        "let a: int[] = [1]\nlet b: int[] = [2]\nlet c: int[] = a + b\npush(c, \"x\")\n",
        LZ_ERR_TYPE, "cannot hold string");
    check_output("let a: int[] = [1]\nlet b: string[] = [\"s\"]\nlet c = a + b\nprintln(c)\n",
                 "[1, \"s\"]\n");
    check_output("let a: int[] = [1]\nlet c = copy(a)\npush(c, 2)\nprintln(a, c)\n",
                 "[1] [1, 2]\n");
    check_runtime_error("let a: int[] = [1]\nlet c = copy(a)\npush(c, \"x\")\n", LZ_ERR_TYPE,
                        "cannot hold");
    check_runtime_error("let a: int[] = [1, 2]\nlet c = a[:1]\npush(c, \"x\")\n", LZ_ERR_TYPE,
                        "cannot hold");
    check_output(
        "let a: int[] = [3, 1, 2]\nsort(a)\nreverse(a)\nprintln(a, pop(a), remove(a, 0), a)\n",
        "[2] 1 3 [2]\n");
}

static void test_type_syntax(void) {
    check_output("type Row = int[]\nlet r: Row = [1]\nlet t: Row[] = [[1], [2]]\nprintln(r, t)\n",
                 "[1] [[1], [2]]\n");
    check_output("let a: any[] = [1, \"s\"]\npush(a, nil)\nprintln(a)\n", "[1, \"s\", nil]\n");
    check_output("let a: (int)[] = [1]\nlet b: ( int | nil )[] = [nil]\nprintln(a, b)\n",
                 "[1] [nil]\n");
    check_compile_error("let a: int[ = [1]\n", "expected ']' in an array type");
    check_compile_error("let a: int[] | string[] = [1]\n",
                        "a union cannot hold two different array types");
    check_compile_error("let a: int[1] = [1]\n", "expected ']' in an array type");
    check_compile_error("let a: nope[] = []\n", "unknown type 'nope'");
    check_output("let a: int[] | int[] = [1]\nprintln(a)\n", "[1]\n");
    check_output("let a: int[] | string = \"s\"\nlet b: int[] | string = [1]\nprintln(a, b)\n",
                 "s [1]\n");
    /* `int[]` is only a type in a type position. */
    check_output("let int = [1, 2]\nprintln(int[1])\n", "2\n");
}

static void test_const_arrays(void) {
    /* A const array is a normal variable (never folded), so each run gets a fresh array. */
    check_output(
        "let i = 0\nwhile i < 3 {\n  const t = [1]\n  push(t, i)\n  println(t)\n  i = i + 1\n}\n",
        "[1, 0]\n[1, 1]\n[1, 2]\n");
    check_compile_error("const xs = [1]\nxs = [2]\n", "cannot assign to constant 'xs'");
    check_output("const N = 3\nconst xs = range(N)\nprintln(xs, len(xs))\n", "[0, 1, 2] 3\n");
    check_output("const xs: int[] = [1]\npush(xs, 2)\nprintln(xs)\n", "[1, 2]\n");
}

static void test_locals_and_loops(void) {
    check_output("{\n  let xs = [1, 2]\n  push(xs, 3)\n  xs[0] = 9\n  println(xs)\n}\n",
                 "[9, 2, 3]\n");
    check_output("let xs = range(5)\nlet i = 0\nlet total = 0\nwhile i < len(xs) {\n"
                 "  total = total + xs[i]\n  i = i + 1\n}\nprintln(total)\n",
                 "10\n");
    check_output("let out = []\nlet i = 0\nwhile i < 5 {\n  push(out, i * i)\n  i = i + "
                 "1\n}\nprintln(out)\n",
                 "[0, 1, 4, 9, 16]\n");
    check_output(
        "let grid = []\nlet r = 0\nwhile r < 3 {\n  let row = []\n  let c = 0\n"
        "  while c < 3 {\n    push(row, r * 3 + c)\n    c = c + 1\n  }\n  push(grid, row)\n"
        "  r = r + 1\n}\nprintln(grid, grid[2][1])\n",
        "[[0, 1, 2], [3, 4, 5], [6, 7, 8]] 7\n");
    check_output("let xs = [5, 3, 8, 1]\nlet i = 0\nwhile i < len(xs) - 1 {\n  let j = 0\n"
                 "  while j < len(xs) - 1 - i {\n    if xs[j] > xs[j + 1] {\n"
                 "      let t = xs[j]\n      xs[j] = xs[j + 1]\n      xs[j + 1] = t\n    }\n"
                 "    j = j + 1\n  }\n  i = i + 1\n}\nprintln(xs)\n",
                 "[1, 3, 5, 8]\n");
    check_output("let xs = [1, 2]\nlet f = xs[0] + xs[1] * 2\nprintln(f, -xs[1], xs[1] - -xs[0])\n",
                 "5 -2 3\n");
    check_output("let xs = [[0]]\nxs[0][0] = xs[0][0] + 1\nxs[0][0] = xs[0][0] + 1\nprintln(xs)\n",
                 "[[2]]\n");
}

static void test_aliasing_and_cycles(void) {
    check_output("let a = [1]\nlet b = a\npush(b, 2)\nprintln(a)\n", "[1, 2]\n");
    check_runtime_error("let a = []\npush(a, a)\n", LZ_ERR_RUNTIME,
                        "cannot store an array inside itself");
    check_runtime_error("let a = [1]\na[0] = a\n", LZ_ERR_RUNTIME,
                        "cannot store an array inside itself");
    check_runtime_error("let a = []\nlet b = [a]\npush(a, b)\n", LZ_ERR_RUNTIME,
                        "cannot store an array inside itself");
    check_runtime_error("let a = []\nlet b = [[a]]\ninsert(a, 0, b)\n", LZ_ERR_RUNTIME,
                        "cannot store an array inside itself");
    check_runtime_error("let a = [0]\nlet b = [a]\nlet c = [b]\nc[0][0][0] = c\n", LZ_ERR_RUNTIME,
                        "cannot store an array inside itself");
    /* Sharing without a cycle is fine, including the same array twice. */
    check_output("let a = [1]\nlet b = [a, a]\nlet c = [b, b]\na[0] = 2\nprintln(c)\n",
                 "[[[2], [2]], [[2], [2]]]\n");
    check_output("let a = []\nlet b = [a]\npush(b, a)\npush(a, 1)\nprintln(b)\n", "[[1], [1]]\n");
    check_output("let a = [1]\npush(a, copy(a))\npush(a, a[:])\nprintln(a)\n",
                 "[1, [1], [1, [1]]]\n");
}

static void test_deep_nesting(void) {
    /* A million levels deep must not overflow the C stack when freed, printed or compared. */
    const char *build = "let a = []\nlet i = 0\nwhile i < 200000 {\n  a = [a]\n  i = i + 1\n}\n";
    Program program;
    LzBuffer text = {0};
    LzWriter writer = lz_writer_from_buffer(&text);
    (void)lz_writer_write_cstr(&writer, build);
    (void)lz_writer_write_cstr(&writer, "let b = copy(a)\nprintln(a == b)\nprintln(len(a))\n");
    run(&program, text.data, 0, 0);
    LZ_CHECK(program.compile_status == LZ_OK);
    LZ_CHECK(program.run_status == LZ_OK);
    LZ_CHECK(program.out.data != NULL && strcmp(program.out.data, "true\n1\n") == 0);
    finish(&program);

    /* Printing a deep array writes the brackets without recursing. */
    lz_buffer_free(&text);
    text = (LzBuffer){0};
    writer = lz_writer_from_buffer(&text);
    (void)lz_writer_write_cstr(&writer, build);
    (void)lz_writer_write_cstr(&writer, "println(a)\n");
    run(&program, text.data, 0, 0);
    LZ_CHECK(program.run_status == LZ_OK);
    /* 200001 pairs of brackets and a newline */
    LZ_CHECK(program.out.length == 2 * 200001 + 1);
    finish(&program);
    lz_buffer_free(&text);
}

static void test_limits_and_budgets(void) {
    Program program;

    run(&program, "let a = []\nwhile true { push(a, 1) }\n", 0, 1 << 16);
    LZ_CHECK(program.run_status == LZ_ERR_HEAP_LIMIT);
    finish(&program);

    run(&program, "let a = range(100000)\n", 0, 1 << 16);
    LZ_CHECK(program.run_status == LZ_ERR_HEAP_LIMIT);
    finish(&program);

    run(&program, "let a = range(1000)\nlet b = a + a\nlet c = b + b\nlet d = c + c\n", 0, 1 << 16);
    LZ_CHECK(program.run_status == LZ_ERR_HEAP_LIMIT);
    finish(&program);

    run(&program, "let a = range(1000)\nsort(a)\nreverse(a)\n", 0, 20000);
    LZ_CHECK(program.run_status == LZ_ERR_HEAP_LIMIT); /* the sort buffer counts too */
    finish(&program);

    run(&program,
        "let a = range(1000)\nlet i = 0\nwhile i < 1000 {\n  a = a + []\n  i = i + 1\n}\n", 5000,
        0);
    LZ_CHECK(program.run_status == LZ_ERR_FUEL);
    finish(&program);

    run(&program, "let a = range(100000)\nlet b = copy(a)\nprintln(a == b)\n", 5000, 0);
    LZ_CHECK(program.run_status == LZ_ERR_FUEL);
    finish(&program);

    run(&program, "let a = range(100000)\nsort(a)\n", 5000, 0);
    LZ_CHECK(program.run_status == LZ_ERR_FUEL);
    finish(&program);

    run(&program,
        "let a = range(1000)\nlet i = 0\nwhile i < 100 {\n  println(a == a)\n  i = i + 1\n}\n",
        5000, 0);
    LZ_CHECK(program.run_status == LZ_OK); /* the same array is equal at once, no walk */
    finish(&program);

    run(&program, "println(range(50) == range(50), contains(range(50), 49))\n", 0, 0);
    LZ_CHECK(program.run_status == LZ_OK);
    finish(&program);

    /* Escalating shared structure must not explode: a diamond DAG is walked once for the cycle
     * check. */
    run(&program,
        "let a = [1]\nlet i = 0\nwhile i < 60 {\n  a = [a, a]\n  i = i + 1\n}\nlet probe = "
        "[]\npush(probe, a)\n"
        "println(len(probe))\n",
        0, 0);
    LZ_CHECK(program.run_status == LZ_OK);
    LZ_CHECK(program.out.data != NULL && strcmp(program.out.data, "1\n") == 0);
    finish(&program);

    run(&program,
        "let a = [1]\nlet i = 0\nwhile i < 60 {\n  a = [a, a]\n  i = i + 1\n}\nprintln(a == a)\n",
        0, 0);
    LZ_CHECK(program.run_status == LZ_OK);
    finish(&program);

    run(&program,
        "let a = [1]\nlet b = [1]\nlet i = 0\nwhile i < 60 {\n  a = [a, a]\n  b = [b, b]\n"
        "  i = i + 1\n}\nprintln(a == b)\n",
        100000, 0);
    LZ_CHECK(program.run_status == LZ_ERR_FUEL); /* exponential walk stops at the budget */
    finish(&program);

    run(&program, "let a = []\nwhile len(a) < 100000 { push(a, 1) }\nprintln(len(a))\n", 0, 0);
    LZ_CHECK(program.run_status == LZ_OK);
    LZ_CHECK(program.out.data != NULL && strcmp(program.out.data, "100000\n") == 0);
    finish(&program);
}

static void test_error_offsets_and_cleanup(void) {
    Program program;
    run(&program, "let xs = [1]\n\nprintln(xs[5])\n", 0, 0);
    LZ_CHECK(program.run_status == LZ_ERR_RUNTIME);
    LZ_CHECK(program.vm.error_offset != LZ_NO_OFFSET);
    LzLocation at = lz_source_locate(&program.source, program.vm.error_offset);
    LZ_CHECK(at.line == 3);
    finish(&program);

    /* Errors in the middle of a literal or call leave nothing behind (finish checks the heap). */
    run(&program, "let xs = [1, [2, 3 / 0]]\n", 0, 0);
    LZ_CHECK(program.run_status == LZ_ERR_RUNTIME);
    finish(&program);
    run(&program, "println([[1], 1 / 0])\n", 0, 0);
    LZ_CHECK(program.run_status == LZ_ERR_RUNTIME);
    finish(&program);
    run(&program, "let g: int[][] = [[1]]\nlet v = [1]\ng[0][0] = v\n", 0, 0);
    LZ_CHECK(program.run_status == LZ_ERR_TYPE);
    finish(&program);
}

static void test_disassembly(void) {
    Program program;
    run(&program, "let xs = [1]\nxs[0] = xs[0 : 1][0]\n", 0, 0);
    LZ_CHECK(program.compile_status == LZ_OK);
    LzBuffer text = {0};
    LzWriter writer = lz_writer_from_buffer(&text);
    lz_disassemble_chunk(&program.chunk, "t", &program.source, &writer);
    LZ_CHECK(text.data != NULL && strstr(text.data, "OP_ARRAY_NEW") != NULL);
    LZ_CHECK(strstr(text.data, "OP_ARRAY_PUSH") != NULL);
    LZ_CHECK(strstr(text.data, "OP_SET_INDEX") != NULL);
    LZ_CHECK(strstr(text.data, "OP_SLICE") != NULL);
    LZ_CHECK(strstr(text.data, "OP_INDEX") != NULL);
    LZ_CHECK(strstr(text.data, "unknown") == NULL);
    lz_buffer_free(&text);
    finish(&program);
}

static void test_malformed_bytecode(void) {
    /* The VM must reject nonsense array instructions instead of crashing. */
    LzOpCode ops[] = {LZ_OP_ARRAY_PUSH, LZ_OP_INDEX, LZ_OP_SET_INDEX, LZ_OP_SLICE};
    for (size_t i = 0; i < sizeof ops / sizeof ops[0]; i++) {
        LzVM vm;
        LzBuffer out = {0};
        LzBuffer err = {0};
        lz_vm_init(&vm, lz_writer_from_buffer(&out), lz_writer_from_buffer(&err));
        LzChunk chunk;
        lz_chunk_init(&chunk);
        (void)lz_chunk_write(&chunk, (uint8_t)ops[i], 0);
        (void)lz_chunk_write(&chunk, (uint8_t)LZ_OP_RETURN, 0);
        LzStatus status = lz_vm_run(&vm, &chunk);
        LZ_CHECK(status == LZ_ERR_RUNTIME);
        LZ_CHECK(strstr(vm.error, "malformed bytecode") != NULL);
        lz_chunk_free(&vm.heap, &chunk);
        lz_vm_free(&vm);
        lz_buffer_free(&out);
        lz_buffer_free(&err);
    }

    /* ARRAY_PUSH onto something that is not an array, and INDEX on a number. */
    LzVM vm;
    LzBuffer out = {0};
    LzBuffer err = {0};
    lz_vm_init(&vm, lz_writer_from_buffer(&out), lz_writer_from_buffer(&err));
    LzChunk chunk;
    lz_chunk_init(&chunk);
    uint16_t one = 0;
    (void)lz_chunk_add_constant(&chunk, lz_value_int(1), &one);
    for (int i = 0; i < 2; i++) {
        (void)lz_chunk_write(&chunk, (uint8_t)LZ_OP_CONSTANT, 0);
        (void)lz_chunk_write_u16(&chunk, one, 0);
    }
    (void)lz_chunk_write(&chunk, (uint8_t)LZ_OP_ARRAY_PUSH, 0);
    (void)lz_chunk_write(&chunk, (uint8_t)LZ_OP_RETURN, 0);
    LZ_CHECK(lz_vm_run(&vm, &chunk) == LZ_ERR_RUNTIME);
    LZ_CHECK(strstr(vm.error, "malformed bytecode: array expected") != NULL);
    lz_chunk_free(&vm.heap, &chunk);
    lz_vm_free(&vm);
    lz_buffer_free(&out);
    lz_buffer_free(&err);
}

static void test_c_api(void) {
    LzVM vm;
    LzBuffer out = {0};
    LzBuffer err = {0};
    lz_vm_init(&vm, lz_writer_from_buffer(&out), lz_writer_from_buffer(&err));

    LzArray *array = lz_array_new(&vm.heap, 0);
    LZ_CHECK(array != NULL);
    for (int i = 0; i < 100; i++) {
        LzValue value = lz_value_int(i);
        LZ_CHECK(lz_array_append(&vm.heap, array, &value));
    }
    LZ_CHECK(array->count == 100 && array->capacity >= 100);
    LZ_CHECK(vm.heap.live_bytes >= sizeof(LzArray) + 100 * sizeof(LzValue));

    LzValue inserted = lz_value_int(-1);
    LZ_CHECK(lz_array_insert_at(&vm.heap, array, 0, &inserted));
    LZ_CHECK(!lz_array_insert_at(&vm.heap, array, 500, &inserted));
    LzValue removed;
    lz_array_remove_at(array, 0, &removed);
    LZ_CHECK(removed.type == LZ_VAL_INT && removed.as.integer == -1);

    LzValue as_value = lz_value_object(&array->base);
    LzType type = lz_type_of_value(&as_value);
    LZ_CHECK(type.kinds == LZ_KIND_ARRAY && type.element == NULL);
    LZ_CHECK(lz_type_matches(&type, &as_value));

    LzType integer = lz_type_of_kinds(LZ_KIND_INT);
    LzType ints;
    LZ_CHECK(lz_vm_array_of(&vm, &integer, &ints) == LZ_OK);
    LzType again;
    LZ_CHECK(lz_vm_array_of(&vm, &integer, &again) == LZ_OK);
    LZ_CHECK(ints.element == again.element && lz_type_equal(&ints, &again));
    LZ_CHECK(lz_type_matches(&ints, &as_value));
    LZ_CHECK(lz_array_conform(array, ints.element));
    LZ_CHECK(array->element == ints.element);

    LzType strings;
    LzType text = lz_type_of_kinds(LZ_KIND_STRING);
    LZ_CHECK(lz_vm_array_of(&vm, &text, &strings) == LZ_OK);
    LZ_CHECK(!lz_type_equal(&ints, &strings));
    LZ_CHECK(!lz_type_matches(&strings, &as_value));
    LZ_CHECK(lz_type_assignable(&ints, &strings) == LZ_ASSIGN_NEVER);
    LzType any_items;
    LZ_CHECK(lz_vm_array_of(&vm, NULL, &any_items) == LZ_OK && any_items.element == NULL);
    LZ_CHECK(lz_type_assignable(&ints, &any_items) == LZ_ASSIGN_ALWAYS);
    LZ_CHECK(lz_type_assignable(&any_items, &ints) == LZ_ASSIGN_MAYBE);
    LZ_CHECK(lz_type_hash(&ints) != lz_type_hash(&strings));

    LzBuffer described = {0};
    LzWriter described_out = lz_writer_from_buffer(&described);
    lz_type_describe(&ints, &described_out);
    LZ_CHECK(described.data != NULL && strcmp(described.data, "int[]") == 0);
    lz_buffer_free(&described);

    /* Every item is released with the array. */
    LzString *text_value = lz_string_new(&vm.heap, "x", 1);
    LzValue boxed = lz_value_object(&text_value->base);
    LzArray *holder = lz_array_new(&vm.heap, 2);
    LZ_CHECK(holder != NULL && lz_array_append(&vm.heap, holder, &boxed));
    lz_object_release(&vm.heap, &text_value->base);
    LZ_CHECK(text_value->base.refcount == 1);
    lz_object_release(&vm.heap, &holder->base);
    lz_object_release(&vm.heap, &array->base);
    LZ_CHECK(vm.heap.live_objects == 0);
    LZ_CHECK(vm.heap.live_bytes == 0);

    /* The heap limit counts the item buffer. */
    lz_vm_set_heap_limit(&vm, 1000);
    LZ_CHECK(lz_array_new(&vm.heap, 1000) == NULL);
    LZ_CHECK(vm.heap.limit_hit);
    LZ_CHECK(lz_array_new(&vm.heap, LZ_ARRAY_MAX_LENGTH + 1) == NULL);

    lz_vm_free(&vm);
    lz_buffer_free(&out);
    lz_buffer_free(&err);
}

int main(void) {
    test_literals_and_printing();
    test_indexing();
    test_slices();
    test_operators();
    test_builtins();
    test_builtin_errors();
    test_failed_calls_change_nothing();
    test_typed_arrays();
    test_typed_assignment_checks();
    test_type_syntax();
    test_const_arrays();
    test_locals_and_loops();
    test_aliasing_and_cycles();
    test_deep_nesting();
    test_limits_and_budgets();
    test_error_offsets_and_cleanup();
    test_disassembly();
    test_malformed_bytecode();
    test_c_api();
    return LZ_TEST_RESULT();
}
