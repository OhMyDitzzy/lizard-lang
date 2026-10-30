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

static void test_short_functions(void) {
    check_output("let sq = fn(n) => n * n\nprintln(sq(7))\n", "49\n");
    check_output("let add = fn(a: int, b: int) -> int => a + b\nprintln(add(2, 3))\n", "5\n");
    check_output("fn make(k) { return fn(x) => x * k }\nprintln(make(3)(5))\n", "15\n");
    check_output("let f = fn() => nil\nprintln(f())\n", "nil\n");
    /* The body may start on the next line, and the result type is checked. */
    check_output("let f = fn(x) =>\n  x + 1\nprintln(f(1))\n", "2\n");
    check_compile_error("let f = fn(a: int) -> string => a\n", "return type mismatch");
    check_compile_error("let f = fn(a)\n", "expected '{' or '=>'");
    check_compile_error("let f = fn(a) => {\n  return a\n}\n", "written without '=>'");
}

static void test_higher_order(void) {
    check_output("println(map([1, 2, 3], fn(n) => n * 2))\n", "[2, 4, 6]\n");
    check_output("println(filter([1, 2, 3, 4], fn(n) => n > 2))\n", "[3, 4]\n");
    check_output("println(reduce([1, 2, 3, 4], fn(a, n) => a + n, 0))\n", "10\n");
    check_output("each([1, 2], fn(n) => println(\"item\", n))\n", "item 1\nitem 2\n");
    check_output("println(find([1, 2, 3], fn(n) => n > 1), find([1], fn(n) => n > 5))\n", "2 nil\n");
    check_output("println(any([1, 2], fn(n) => n > 1), any([1], fn(n) => n > 1))\n", "true false\n");
    check_output("println(all([1, 2], fn(n) => n > 0), all([1, 2], fn(n) => n > 1))\n", "true false\n");
    /* Empty arrays, and the neutral answers. */
    check_output("println(map([], fn(n) => n), filter([], fn(n) => n))\n", "[] []\n");
    check_output("println(reduce([], fn(a, n) => a + n, 7), any([], fn(n) => n), all([], fn(n) => n))\n",
                 "7 false true\n");
    /* Only nil and false are false. */
    check_output("println(filter([0, nil, false, \"\", []], fn(x) => x))\n", "[0, \"\", []]\n");
    /* A named function, a builtin and a closure are all callbacks. */
    check_output("fn double(n) { return n * 2 }\nprintln(map([1, 2], double), map([1, 2], string))\n",
                 "[2, 4] [\"1\", \"2\"]\n");
    check_output("let k = 10\nprintln(map([1, 2], fn(x) => x + k))\n", "[11, 12]\n");
    /* A callback that runs a callback. */
    check_output("println(map([[1, 2], [3]], fn(r) => map(r, fn(x) => x * 2)))\n", "[[2, 4], [6]]\n");
    /* filter keeps the element type of an int[]: later stores are still checked. */
    check_runtime_error("let xs: int[] = [1, 2, 3]\nlet ys = filter(xs, fn(n) => n > 1)\npush(ys, \"x\")\n",
                        LZ_ERR_TYPE, "int");
}

static void test_methods(void) {
    check_output("let xs = [3, 1]\nxs.push(2)\nprintln(xs, xs.len())\n", "[3, 1, 2] 3\n");
    check_output("let nums = [1, 2, 3, 4]\n"
                 "println(nums.filter(fn(n) => n > 1).map(fn(n) => n * 10))\n",
                 "[20, 30, 40]\n");
    check_output("fn double(n) { return n * 2 }\nprintln(5.double(), 2.double().double())\n", "10 8\n");
    /* A chain continues on the next line. */
    check_output("let xs = [1, 2, 3]\nprintln(xs\n  .filter(fn(n) => n > 1)\n  .len())\n", "2\n");
    /* Spread and named arguments after the receiver. */
    check_output("fn add3(a, b = 10, c = 100) { return a + b + c }\n"
                 "println(1.add3(), 1.add3(c = 5), 1.add3(*[2, 3]))\n",
                 "111 16 6\n");
    check_output("println(\"abc\".len())\n", "3\n");
}

static void test_callback_mutation(void) {
    /* Pushing while walking visits only the items that were there at the start. */
    check_output("let a = [1, 2]\neach(a, fn(x) => push(a, x))\nprintln(a)\n", "[1, 2, 1, 2]\n");
    /* Shrinking stops the walk where the array now ends. */
    check_output("let a = [1, 2, 3]\neach(a, fn(x) => pop(a))\nprintln(a)\n", "[1]\n");
    check_output("let a = [1, 2, 3]\nprintln(map(a, fn(x) => clear(a)))\n", "[nil]\n");
}

static void test_callback_errors(void) {
    /* The error is reported where it happened, inside the callback. */
    check_fails("let xs = [1, 0]\n\nprintln(map(xs,\n  fn(n) => 10 // n))\n", LZ_ERR_RUNTIME,
                "division by zero", 4);
    check_fails("fn f(n) {\n  return n + nope\n}\nprintln(1)\neach([1], f)\n", LZ_ERR_RUNTIME,
                "undefined variable 'nope'", 2);
    check_fails("println(map([1, 2], fn(a, b) => a))\n", LZ_ERR_ARITY, "expected 2 arguments, got 1", 0);
    check_fails("println(map([1, 2], 5))\n", LZ_ERR_RUNTIME, "expected a function", 0);
    check_fails("println(map(5, fn(x) => x))\n", LZ_ERR_RUNTIME, "expected an array", 0);
    check_fails("fn deep(n) { return map([n], fn(x) => deep(x + 1)) }\ndeep(0)\n",
                LZ_ERR_STACK_OVERFLOW, "callbacks nested too deeply", 0);

    /* The stack trace goes through the callback to the call that started it. */
    Program program;
    run(&program, "fn risky(n) {\n  return map([n], fn(v) => v + nope)\n}\neach([5], risky)\n", 0, 0);
    LZ_CHECK(program.run_status == LZ_ERR_RUNTIME);
    LZ_CHECK(program.vm.trace_count == 3);
    if (program.vm.trace_count == 3) {
        LZ_CHECK(strcmp(program.vm.trace[1].name, "risky") == 0);
        LZ_CHECK(strcmp(program.vm.trace[2].name, "<script>") == 0);
        LZ_CHECK(lz_source_locate(&program.source, program.vm.trace[1].offset).line == 2);
        LZ_CHECK(lz_source_locate(&program.source, program.vm.trace[2].offset).line == 4);
    }
    finish(&program); /* and nothing leaked on the way out */

    /* Callbacks spend fuel like any other code. */
    run(&program, "let big = range(1000)\nprintln(len(map(big, fn(x) => x + 1)))\n", 2000, 0);
    LZ_CHECK(program.run_status == LZ_ERR_FUEL);
    finish(&program);
}

static void test_method_errors(void) {
    Program program;
    run(&program, "let xs = [1]\nxs.pusj(2)\n", 0, 0);
    LZ_CHECK(program.run_status == LZ_ERR_RUNTIME);
    LZ_CHECK(strstr(program.vm.error, "undefined variable 'pusj'") != NULL);
    const LzNameHint *hint = lz_diagnostics_find_hint(&program.diagnostics, program.vm.error_offset);
    LZ_CHECK(hint != NULL);
    if (hint != NULL) {
        LZ_CHECK(hint->is_method && hint->is_call);
        LZ_CHECK(hint->suggestions.count >= 1 && strcmp(hint->suggestions.names[0], "push") == 0);
    }
    finish(&program);

    check_compile_error("let xs = [1]\nprintln(xs.len)\n", "expected '(' after the method name");
    check_compile_error("let xs = [1]\nprintln(xs.)\n", "expected a method name after '.'");
}

int main(void) {
    test_short_functions();
    test_higher_order();
    test_methods();
    test_callback_mutation();
    test_callback_errors();
    test_method_errors();
    return LZ_TEST_RESULT();
}
