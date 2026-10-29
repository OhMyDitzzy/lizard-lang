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

static LzStatus five(LzVM *vm, int argc, const LzValue *argv, LzValue *result) {
    (void)vm;
    (void)argc;
    (void)argv;
    *result = lz_value_int(5);
    return LZ_OK;
}

static LzStatus text_native(LzVM *vm, int argc, const LzValue *argv, LzValue *result) {
    (void)argc;
    (void)argv;
    LzString *text = lz_string_new(&vm->heap, "s", 1);
    if (text == NULL) {
        return lz_vm_out_of_memory(vm);
    }
    *result = lz_value_object(&text->base);
    return LZ_OK;
}

static void define_host(LzVM *vm, const char *name, LzNativeFn fn) {
    LzString *key = lz_vm_intern(vm, name, strlen(name));
    LzNative *native = lz_native_new(&vm->heap, key, fn, 0, 0);
    (void)lz_vm_define_global(vm, key, lz_value_object(&native->base));
    lz_object_release(&vm->heap, &native->base);
}

static void define_five(LzVM *vm) {
    define_host(vm, "five", five);
    define_host(vm, "text", text_native);
}

/* Fuel and heap limit of 0 mean unlimited. */
static void run(Program *program, const char *text, uint64_t fuel, size_t heap_limit) {
    memset(program, 0, sizeof *program);
    lz_vm_init(&program->vm, lz_writer_from_buffer(&program->out),
               lz_writer_from_buffer(&program->err));
    LZ_CHECK(lz_builtin_install(&program->vm) == LZ_OK);
    define_five(&program->vm);

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
    LZ_CHECK(program->vm.heap.live_objects == 0);
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
        fprintf(stderr, "program:\n%s\nexpected: [%s]\ngot: [%s]\n", text, expected,
                program.out.data != NULL ? program.out.data : "(none)");
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

/* The program compiles, prints `expected` and then fails at run time with `message`. */
static void check_runtime_error(const char *text, const char *expected, const char *message) {
    Program program;
    run(&program, text, 0, 0);
    LZ_CHECK(program.compile_status == LZ_OK);
    LZ_CHECK(program.run_status == LZ_ERR_RUNTIME);
    LZ_CHECK(strstr(program.vm.error, message) != NULL);
    LZ_CHECK(strcmp(program.out.data != NULL ? program.out.data : "", expected) == 0);
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

static void test_expressions(void) {
    check_output("println(\"hi\", 1 + 2 * 3)\n", "hi 7\n");
    check_output("println(10 - 4 - 3, 2 * (3 + 4), -5 + 2, - -3)\n", "3 14 -3 3\n");
    check_output("println(7 / 2, 7 // 2, -7 // 2, -7 % 3, 7 % -3)\n", "3.5 3 -4 2 -2\n");
    check_output("println(1 < 2, 2 <= 2, 3 > 4, 4 >= 5, 1 == 1.0, 1 != 2, !nil, !0)\n",
                 "true true false false true true true false\n");
    check_output("println(\"a\" + \"b\" + \"c\", \"x\" == \"x\", nil == false)\n", "abc true false\n");
    check_output("println(nil or 5, 1 and 2, false or nil, nil and 1)\n", "5 2 nil nil\n");
    check_output("println(1 < 2 and 2 < 3, 1 > 2 or 2 > 1)\n", "true true\n");
    check_output("println()\nprint(\"a\")\nprint(\"b\")\nprintln()\n", "\nab\n");
}

static void test_short_circuit(void) {
    check_output("println(false and println(\"side\"))\n", "false\n");
    check_output("println(true or println(\"side\"))\n", "true\n");
    check_output("println(true and println(\"side\"))\n", "side\nnil\n");
}

static void test_globals_and_assignment(void) {
    check_output("let a = 2\nlet b = a * 3\nprintln(a, b)\n", "2 6\n");
    check_output("let x = 1\nx = x + 1\nx = x * 10\nprintln(x)\n", "20\n");
    check_output("let n\nprintln(n)\nn = \"set\"\nprintln(n)\n", "nil\nset\n");
    check_output("let x = 1; let y = 2; println(x + y)\n", "3\n");
    check_output("let print = 1\nprintln(print)\n", "1\n");
}

static void test_blocks_and_locals(void) {
    check_output("let x = 1\n"
                 "{\n"
                 "  let x = 2\n"
                 "  println(x)\n"
                 "  {\n"
                 "    let x = 3\n"
                 "    println(x)\n"
                 "  }\n"
                 "  println(x)\n"
                 "}\n"
                 "println(x)\n",
                 "2\n3\n2\n1\n");
    check_output("let g = 10\n{\n  let a = g + 1\n  let b = a * 2\n  g = b\n  println(a, b)\n}\n"
                 "println(g)\n",
                 "11 22\n22\n");
    check_output("{\n  let a = 1\n  a = a + 1\n  { let b = a + 10\n    a = b }\n  println(a)\n}\n",
                 "12\n");
    check_output("{ let a = 1 }\n{ let a = 2\n println(a) }\n", "2\n");
}

static void test_control_flow(void) {
    check_output("let n = 5\n"
                 "if n < 3 { println(\"small\") } else if n < 10 { println(\"medium\") }"
                 " else { println(\"large\") }\n",
                 "medium\n");
    check_output("if false {\n  println(\"a\")\n}\nelse {\n  println(\"b\")\n}\n", "b\n");
    check_output("if true { println(\"yes\") }\nprintln(\"after\")\n", "yes\nafter\n");
    check_output("if nil { println(\"no\") }\nprintln(\"after\")\n", "after\n");
    check_output("if 0 { println(\"zero is truthy\") }\n", "zero is truthy\n");

    check_output("let i = 1\nlet sum = 0\nwhile i <= 10 {\n  sum = sum + i\n  i = i + 1\n}\n"
                 "println(sum)\n",
                 "55\n");
    check_output("let total = 0\nlet i = 0\nwhile i < 3 {\n  let j = 0\n  while j < 3 {\n"
                 "    total = total + i * j\n    j = j + 1\n  }\n  i = i + 1\n}\nprintln(total)\n",
                 "9\n");
    check_output("let i = 0\nwhile i < 3000 {\n  let a = i\n  let b = a + 1\n  i = b\n}\n"
                 "println(i)\n",
                 "3000\n");
    check_output("while false { println(\"never\") }\nprintln(\"done\")\n", "done\n");
}

static void test_fizzbuzz(void) {
    check_output("let i = 1\n"
                 "while i <= 15 {\n"
                 "  if i % 15 == 0 {\n    println(\"FizzBuzz\")\n  } else if i % 3 == 0 {\n"
                 "    println(\"Fizz\")\n  } else if i % 5 == 0 {\n    println(\"Buzz\")\n"
                 "  } else {\n    println(i)\n  }\n  i = i + 1\n}\n",
                 "1\n2\nFizz\n4\nBuzz\nFizz\n7\n8\nFizz\nBuzz\n11\nFizz\n13\n14\nFizzBuzz\n");
}

static void test_constants(void) {
    Program program;

    run(&program, "const A = 2 * 3 + 1\nprintln(A, A + 1)\n", 0, 0);
    LZ_CHECK(program.compile_status == LZ_OK && program.run_status == LZ_OK);
    LZ_CHECK(program.out.data != NULL && strcmp(program.out.data, "7 8\n") == 0);
    LZ_CHECK(program.vm.globals.count == 2);
    finish(&program);

    run(&program, "const G = \"he\" + \"llo\"\nprintln(G, G + \"!\")\n", 0, 0);
    LZ_CHECK(program.out.data != NULL && strcmp(program.out.data, "hello hello!\n") == 0);
    LZ_CHECK(program.vm.globals.count == 2);
    finish(&program);

    run(&program, "const C = five() + 1\nprintln(C)\n", 0, 0);
    LZ_CHECK(program.compile_status == LZ_OK && program.run_status == LZ_OK);
    LZ_CHECK(program.out.data != NULL && strcmp(program.out.data, "6\n") == 0);
    LZ_CHECK(program.vm.globals.count == 3);
    finish(&program);

    check_output("{\n  const K = 3\n  const L = K * K\n  println(K, L)\n}\n", "3 9\n");
    check_output("{\n  const R = five()\n  println(R)\n}\n", "5\n");
    check_output("const B = true\nconst N = nil\nprintln(B, N, !B)\n", "true nil false\n");
    check_output("const A = 1\n{\n  let A = 2\n  println(A)\n}\nprintln(A)\n", "2\n1\n");
    check_output("const MAX = 3\nlet i = 0\nwhile i < MAX { i = i + 1 }\nprintln(i)\n", "3\n");
}

static void test_syntax_forms(void) {
    check_output("let t = 1 +\n  2\nprintln(t)\n", "3\n");
    check_output("let t =\n  4\nprintln(t)\n", "4\n");
    check_output("println(1,\n  2,\n  3,\n)\n", "1 2 3\n");
    check_output("# comment\n\n\nprintln(1) # trailing\n\n", "1\n");
    check_output("let a = 1; ; let b = 2;\nprintln(a + b);\n", "3\n");
    check_output("{ println(1) }\n", "1\n");
    check_output("", "");
    check_output("let a = 1\nlet b = a and\n  2\nprintln(b)\n", "2\n");
}

static void test_compile_errors(void) {
    check_compile_error("let x = 1\nlet x = 2\n", "already declared");
    check_compile_error("{ let a = 1\n let a = 2 }\n", "already declared");
    check_compile_error("const X = 1\nX = 2\n", "cannot assign to constant 'X'");
    check_compile_error("const X = five()\nX = 2\n", "cannot assign to constant 'X'");
    check_compile_error("{ const X = 1\n X = 2 }\n", "cannot assign to constant 'X'");
    check_compile_error("println = 1\n", "cannot assign to builtin 'println'");
    check_compile_error("1 = 2\n", "invalid assignment target");
    check_compile_error("let x = \n", "expected expression");
    check_compile_error("let = 1\n", "expected variable name");
    check_compile_error("const = 1\n", "expected constant name");
    check_compile_error("const X\n", "expected '=' after constant name");
    check_compile_error("println(1 2)\n", "expected ')'");
    check_compile_error("println(1) println(2)\n", "expected end of statement");
    check_compile_error("if true println(1)\n", "expected '{' after condition");
    check_compile_error("while true println(1)\n", "expected '{' after condition");
    check_compile_error("if true { println(1)\n", "expected '}' after block");
    check_compile_error("if true { } else println(1)\n", "expected '{' or 'if' after 'else'");
    check_compile_error("}\n", "unexpected '}'");
    check_compile_error("(1 + 2\n", "expected ')' after expression");
    check_compile_error("let x = 12abc\n", "malformed number");
    check_compile_error("let x = \"oops\n", "unterminated string");
    check_compile_error("let x = @\n", "unexpected character");
    check_compile_error("const Z = 1 / 0\n", "division by zero");
    check_compile_error("const Z = 9223372036854775807 + 1\n", "integer overflow");
    check_compile_error("const Z = \"a\" + 1\n", "operator '+' cannot be applied to \"a\" and 1");
}

/* Sequential execution: an undefined name fails when its statement runs, not before. */
static void test_sequential_execution(void) {
    check_runtime_error("println(\"Hello, World!\")\nprintj(\"Hello\")\n", "Hello, World!\n",
                        "undefined variable 'printj'");
    check_runtime_error("println(1)\nprintln(y)\nprintln(2)\n", "1\n", "undefined variable 'y'");
    check_runtime_error("println(1)\ny = 1\nprintln(2)\n", "1\n", "undefined variable 'y'");
    check_runtime_error("println(1)\n{ let a = a }\n", "1\n", "undefined variable 'a'");
    /* Never reached: no error. */
    check_output("if false { println(nope) }\nprintln(1)\n", "1\n");
    /* Defined later, used inside a function that only runs afterwards. */
    check_output("fn f() { g() }\nfn g() { println(7) }\nf()\n", "7\n");
}

static void test_recovery_reports_several_errors(void) {
    Program program;
    run(&program, "let x = 1\nlet x = 2\nconst Z = 1 / 0\nx = = 3\nprintln(x)\n", 0, 0);
    LZ_CHECK(program.compile_status == LZ_ERR_COMPILE);
    LZ_CHECK(program.diagnostics.count == 3);
    LZ_CHECK(strstr(program.diagnostics.items[0].message, "already declared") != NULL);
    LZ_CHECK(strstr(program.diagnostics.items[1].message, "division by zero") != NULL);
    LZ_CHECK(strstr(program.diagnostics.items[2].message, "expected expression") != NULL);

    LzBuffer text = {0};
    LzWriter writer = lz_writer_from_buffer(&text);
    lz_diagnostics_write(&program.diagnostics, &program.source, &writer);
    LZ_CHECK(text.data != NULL && strstr(text.data, "t.lz:2:5: error:") != NULL);
    lz_buffer_free(&text);
    finish(&program);

    run(&program, "let a = @\nlet b = @\nlet c = @\n", 0, 0);
    LZ_CHECK(program.diagnostics.count == 3);
    finish(&program);
}

static void test_many_errors_are_capped(void) {
    char text[2048];
    size_t length = 0;
    for (int i = 0; i < 40; i++) {
        length += (size_t)snprintf(text + length, sizeof text - length, "let a = @\n");
    }
    Program program;
    run(&program, text, 0, 0);
    LZ_CHECK(program.compile_status == LZ_ERR_COMPILE);
    LZ_CHECK(program.diagnostics.count == LZ_MAX_DIAGNOSTICS);
    LZ_CHECK(program.diagnostics.dropped == 40 - LZ_MAX_DIAGNOSTICS);

    LzBuffer written = {0};
    LzWriter writer = lz_writer_from_buffer(&written);
    lz_diagnostics_write(&program.diagnostics, &program.source, &writer);
    LZ_CHECK(written.data != NULL && strstr(written.data, "24 more error(s)") != NULL);
    lz_buffer_free(&written);
    finish(&program);
}

static void test_limits_in_the_compiler(void) {
    char text[4096];
    size_t length = 0;

    length = (size_t)snprintf(text, sizeof text, "println(");
    for (int i = 0; i < 300; i++) {
        length += (size_t)snprintf(text + length, sizeof text - length, "1,");
    }
    snprintf(text + length, sizeof text - length, "1)\n");
    check_compile_error(text, "too many arguments");

    length = 0;
    for (int i = 0; i < 300; i++) {
        text[length++] = '(';
    }
    text[length++] = '1';
    for (int i = 0; i < 300; i++) {
        text[length++] = ')';
    }
    text[length++] = '\n';
    text[length] = '\0';
    check_compile_error(text, "nested too deeply");

    length = 0;
    for (int i = 0; i < 200; i++) {
        length += (size_t)snprintf(text + length, sizeof text - length, "{");
    }
    for (int i = 0; i < 200; i++) {
        length += (size_t)snprintf(text + length, sizeof text - length, "}");
    }
    check_compile_error(text, "nested too deeply");

    length = 0;
    length += (size_t)snprintf(text + length, sizeof text - length, "{\n");
    for (int i = 0; i < 300; i++) {
        length += (size_t)snprintf(text + length, sizeof text - length, "let v%d = 1\n", i);
    }
    check_compile_error(text, "too many local variables");
}

static void test_runtime_behaviour(void) {
    Program program;

    run(&program, "println(\"before\")\nprintln(1 / 0)\n", 0, 0);
    LZ_CHECK(program.compile_status == LZ_OK);
    LZ_CHECK(program.run_status == LZ_ERR_RUNTIME);
    LZ_CHECK(strstr(program.vm.error, "division by zero") != NULL);
    LZ_CHECK(program.vm.error_offset == 28);
    LZ_CHECK(program.source.text[program.vm.error_offset] == '/');
    LZ_CHECK(program.out.data != NULL && strcmp(program.out.data, "before\n") == 0);
    finish(&program);

    run(&program, "while true { }\n", 1000, 0);
    LZ_CHECK(program.run_status == LZ_ERR_FUEL);
    finish(&program);

    run(&program, "let s = \"ab\"\nwhile true { s = s + s }\n", 100000, 65536);
    LZ_CHECK(program.run_status == LZ_ERR_HEAP_LIMIT);
    finish(&program);

    run(&program, "let i = 0\nwhile i < 100 { i = i + 1 }\nprintln(i)\n", 5, 0);
    LZ_CHECK(program.run_status == LZ_ERR_FUEL);
    finish(&program);

    run(&program, "const S = 5 / 0\n", 3, 0);
    LZ_CHECK(program.compile_status == LZ_ERR_COMPILE);
    LZ_CHECK(program.vm.fuel == 3);
    finish(&program);
}

/* Test programs are assembled from repeated pieces. */
static void append(LzBuffer *text, const char *piece) {
    LzWriter writer = lz_writer_from_buffer(text);
    (void)lz_writer_write_cstr(&writer, piece);
}

static void test_long_boolean_chains(void) {
    LzBuffer and_chain = {0};
    LzBuffer or_chain = {0};
    append(&and_chain, "println(");
    append(&or_chain, "println(");
    for (int i = 0; i < 600; i++) {
        append(&and_chain, "1 and ");
        append(&or_chain, "nil or ");
    }
    append(&and_chain, "2)\n");
    append(&or_chain, "7)\n");
    check_output(and_chain.data, "2\n");
    check_output(or_chain.data, "7\n");
    lz_buffer_free(&and_chain);
    lz_buffer_free(&or_chain);

    /* Parsing a chain as a loop must not change which operand is returned. */
    check_output("println(nil or 2 and 3, 1 and nil or 5, 0 and \"s\", nil and 1 or 2 and 3)\n",
                 "3 5 s 3\n");
    check_output("println(false or false or 4, 1 and 2 and nil and 3, nil or nil or nil)\n",
                 "4 nil nil\n");
}

static void else_if_chain(LzBuffer *text, size_t branches, size_t selected) {
    char piece[96];
    (void)snprintf(piece, sizeof piece, "let x = %zu\nif x == -1 { println(-1) }\n", selected);
    append(text, piece);
    for (size_t i = 0; i < branches; i++) {
        (void)snprintf(piece, sizeof piece, "else if x == %zu { println(%zu) }\n", i, i);
        append(text, piece);
    }
}

static void test_long_else_if_chains(void) {
    LzBuffer text = {0};
    else_if_chain(&text, 2000, 1999);
    check_output(text.data, "1999\n");
    lz_buffer_free(&text);

    /* Far past the 16-bit jump distance: a compile error, never a crash from deep recursion. */
    memset(&text, 0, sizeof text);
    else_if_chain(&text, 300000, 5);
    Program program;
    run(&program, text.data, 0, 0);
    LZ_CHECK(program.compile_status == LZ_ERR_COMPILE);
    finish(&program);
    lz_buffer_free(&text);
}

static void test_int64_min_literal(void) {
    check_output("println(-9223372036854775808)\n", "-9223372036854775808\n");
    check_output("println(-0x8000000000000000, -0b1000000000000000000000000000000000000000000000"
                 "000000000000000000)\n",
                 "-9223372036854775808 -9223372036854775808\n");
    check_output("const M = -9223372036854775808\nprintln(M, M + 1)\n",
                 "-9223372036854775808 -9223372036854775807\n");
    check_output("println(-9223372036854775807 - 1 == -9223372036854775808)\n", "true\n");
    check_output("println(-1 - -9223372036854775808)\n", "9223372036854775807\n");

    /* Positive 2^63 does not fit in an int. */
    check_compile_error("println(9223372036854775808)\n", "out of range");
    check_compile_error("println(1 - 9223372036854775808)\n", "out of range");
    check_compile_error("println(!9223372036854775808)\n", "out of range");
    check_compile_error("println(-9223372036854775809)\n", "out of range");

    Program program;
    run(&program, "println(-(-9223372036854775808))\n", 0, 0);
    LZ_CHECK(program.compile_status == LZ_OK);
    LZ_CHECK(program.run_status == LZ_ERR_RUNTIME);
    LZ_CHECK(strstr(program.vm.error, "overflow") != NULL);
    finish(&program);
}

static void test_constants_are_shared(void) {
    Program program;
    run(&program, "println(1, 1, 1, \"a\", \"a\", 1.5, 1.5, 1.0)\n", 0, 0);
    LZ_CHECK(program.compile_status == LZ_OK);
    LZ_CHECK(program.chunk.constant_count == 5); /* println, 1, "a", 1.5, 1.0 */
    finish(&program);

    check_output("println(1, 1.0, 1)\n", "1 1.0 1\n");

    /* A string literal is not the interned name of a variable, whichever comes first. */
    check_output("println(\"x\")\nlet x = 5\nprintln(x)\nx = 6\nprintln(x)\n", "x\n5\n6\n");
    check_output("let y = 1\nprintln(\"y\", y)\ny = 2\nprintln(y)\n", "y 1\n2\n");

    /* 0.0 and -0.0 are different constants. */
    check_output("println(0.0)\nconst Z = -0.0\nprintln(Z, 0.0)\n", "0.0\n-0.0 0.0\n");
}

static void test_many_references_fit_in_one_chunk(void) {
    LzBuffer text = {0};
    append(&text, "let x = 1\n");
    for (int i = 0; i < 70000; i++) {
        append(&text, "x\n");
    }
    append(&text, "println(x)\n");
    check_output(text.data, "1\n");
    lz_buffer_free(&text);

    memset(&text, 0, sizeof text);
    for (int i = 0; i < 70000; i++) {
        append(&text, "7\n\"s\"\n");
    }
    append(&text, "println(8)\n");
    check_output(text.data, "8\n");
    lz_buffer_free(&text);

    /* Only distinct constants count towards the limit of a chunk. */
    memset(&text, 0, sizeof text);
    char piece[32];
    for (size_t i = 0; i < 70000; i++) {
        (void)snprintf(piece, sizeof piece, "%zu\n", i);
        append(&text, piece);
    }
    check_compile_error(text.data, "too many constants");
    lz_buffer_free(&text);
}

/* Each block folds a const whose literals are then dropped from the constant table. */
static void test_folded_consts_with_a_shared_table(void) {
    LzBuffer text = {0};
    LzBuffer expected = {0};
    char piece[96];
    for (size_t i = 0; i < 3000; i++) {
        (void)snprintf(piece, sizeof piece, "{ const C = %zu + 1\n  println(C) }\n", i + 1000);
        append(&text, piece);
        (void)snprintf(piece, sizeof piece, "%zu\n", i + 1001);
        append(&expected, piece);
    }
    Program program;
    run(&program, text.data, 0, 0);
    LZ_CHECK(program.compile_status == LZ_OK);
    LZ_CHECK(program.run_status == LZ_OK);
    LZ_CHECK(program.out.data != NULL && strcmp(program.out.data, expected.data) == 0);
    LZ_CHECK(program.chunk.constant_count == 3001); /* println and the 3000 folded values */
    finish(&program);
    lz_buffer_free(&text);
    lz_buffer_free(&expected);
}

static bool same_constant(const LzValue *x, const LzValue *y) {
    if (x->type != y->type) {
        return false;
    }
    switch (x->type) {
    case LZ_VAL_NIL:
        return true;
    case LZ_VAL_BOOL:
        return x->as.boolean == y->as.boolean;
    case LZ_VAL_INT:
        return x->as.integer == y->as.integer;
    case LZ_VAL_FLOAT:
        return memcmp(&x->as.number, &y->as.number, sizeof x->as.number) == 0;
    case LZ_VAL_OBJECT: {
        const LzString *a = (const LzString *)x->as.object;
        const LzString *b = (const LzString *)y->as.object;
        return x->as.object == y->as.object ||
               (x->as.object->type == LZ_OBJ_STRING && y->as.object->type == LZ_OBJ_STRING &&
                a->length == b->length && memcmp(a->data, b->data, a->length) == 0);
    }
    }
    return false;
}

/* Literals, names and folded consts drawn from small pools collide a lot in the index. */
static void test_constant_table_has_no_duplicates(void) {
    LzBuffer text = {0};
    char piece[96];
    uint32_t state = 12345;
    for (int i = 0; i < 4000; i++) {
        state = state * 1664525u + 1013904223u;
        unsigned pick = (state >> 8) % 40;
        unsigned other = (state >> 16) % 40;
        switch (state % 5) {
        case 0:
            (void)snprintf(piece, sizeof piece, "%u\n", pick);
            break;
        case 1:
            (void)snprintf(piece, sizeof piece, "{ const C = %u + %u\n  println(C) }\n", pick, other);
            break;
        case 2:
            (void)snprintf(piece, sizeof piece, "\"s%u\"\n", pick);
            break;
        case 3:
            (void)snprintf(piece, sizeof piece, "%u.5\n", pick);
            break;
        default:
            (void)snprintf(piece, sizeof piece, "{ const S = \"a%u\" + \"b%u\"\n  println(S) }\n", pick,
                           other);
            break;
        }
        append(&text, piece);
    }

    Program program;
    run(&program, text.data, 0, 0);
    LZ_CHECK(program.compile_status == LZ_OK);
    LZ_CHECK(program.run_status == LZ_OK);
    size_t duplicates = 0;
    for (size_t i = 0; i < program.chunk.constant_count; i++) {
        for (size_t j = i + 1; j < program.chunk.constant_count; j++) {
            if (same_constant(&program.chunk.constants[i], &program.chunk.constants[j])) {
                duplicates++;
            }
        }
    }
    LZ_CHECK(duplicates == 0);
    finish(&program);
    lz_buffer_free(&text);
}

static void test_fuel_covers_large_strings(void) {
    const char *doubling = "let s = \"x\"\nlet i = 0\nwhile i < 20 { s = s + s\n i = i + 1 }\n";

    Program program;
    run(&program, doubling, 5000, 0);
    LZ_CHECK(program.compile_status == LZ_OK);
    LZ_CHECK(program.run_status == LZ_ERR_FUEL);
    finish(&program);

    run(&program, doubling, 0, 0);
    LZ_CHECK(program.run_status == LZ_OK);
    finish(&program);
}

static void test_fuel_covers_every_large_string_operation(void) {
    const char *doubling = "let s = \"x\"\nlet i = 0\nwhile i < 20 { s = s + s\n i = i + 1 }\n";
    const char *tails[] = {
        "while true { let t = s + s }\n",
        "while true { println(s) }\n",
        "let copy = s + \"\"\nwhile true { let same = s == copy }\n",
        "let copy = s + \"\"\nwhile true { let same = s != copy }\n",
    };

    for (size_t i = 0; i < sizeof tails / sizeof tails[0]; i++) {
        size_t size = strlen(doubling) + strlen(tails[i]) + 1;
        char *text = malloc(size);
        (void)snprintf(text, size, "%s%s", doubling, tails[i]);

        /* 1 MiB strings: far more fuel is spent on bytes than on the few instructions per turn. */
        Program program;
        run(&program, text, 400000, 0);
        LZ_CHECK(program.compile_status == LZ_OK);
        LZ_CHECK(program.run_status == LZ_ERR_FUEL);
        LZ_CHECK(program.vm.instructions < 2000);
        finish(&program);
        free(text);
    }

    check_output("let s = \"x\"\nprintln(s == s, s == \"x\", s != \"y\")\n", "true true true\n");
}

static void test_else_if_branches_report_their_own_line(void) {
    const char *text = "let i = 0\nwhile true {\n  if i == 100 { println(1) }\n"
                       "  else if i == 200 { println(2) }\n  else if i == 300 { println(3) }\n"
                       "  i = i + 1\n}\n";
    Program program;
    run(&program, text, 50, 0);
    LZ_CHECK(program.compile_status == LZ_OK);

    /* Conditional jumps in emission order: the while test, then one per if / else if branch. */
    unsigned lines[8];
    size_t count = 0;
    for (size_t i = 0; i < program.chunk.count; i += lz_opcode_length(program.chunk.code[i])) {
        if (program.chunk.code[i] == LZ_OP_JUMP_IF_FALSE && count < 8) {
            lines[count++] =
                lz_source_locate(&program.source, program.chunk.offsets[i]).line;
        }
    }
    LZ_CHECK(count == 4);
    if (count == 4) {
        LZ_CHECK(lines[0] == 2 && lines[1] == 3 && lines[2] == 4 && lines[3] == 5);
    }
    finish(&program);
}

static void test_heap_limit_status_while_compiling(void) {
    LzBuffer names = {0};
    char piece[48];
    for (int i = 0; i < 400; i++) {
        (void)snprintf(piece, sizeof piece, "name_number_%d\n", i);
        append(&names, piece);
    }

    LzBuffer out = {0};
    LzVM vm;
    lz_vm_init(&vm, lz_writer_from_buffer(&out), lz_writer_from_buffer(&out));
    LZ_CHECK(lz_builtin_install(&vm) == LZ_OK);
    lz_vm_set_heap_limit(&vm, vm.heap.live_bytes + 2000);

    /* Every name is interned before it is rejected, so the limit is hit inside the lexer's callers. */
    LzSource source;
    lz_source_from_string(&source, "t.lz", names.data);
    LzChunk chunk;
    lz_chunk_init(&chunk);
    LzDiagnostics diagnostics;
    LZ_CHECK(lz_compile(&vm, &source, &chunk, &diagnostics) == LZ_ERR_HEAP_LIMIT);
    LZ_CHECK(strstr(vm.error, "heap limit") != NULL);

    lz_chunk_free(&vm.heap, &chunk);
    lz_vm_release_roots(&vm);
    LZ_CHECK(vm.heap.live_objects == 0);
    lz_vm_free(&vm);
    lz_buffer_free(&out);
    lz_buffer_free(&names);
}

static void test_const_folding_leaves_no_code(void) {
    Program program;
    run(&program, "const A = 1 + 2\n", 0, 0);
    LZ_CHECK(program.compile_status == LZ_OK);
    LZ_CHECK(program.chunk.count == 1);
    LZ_CHECK(program.chunk.code[0] == LZ_OP_RETURN);
    LZ_CHECK(program.chunk.constant_count == 0);
    finish(&program);

    run(&program, "const A = \"a\" + \"b\"\nprintln(A)\n", 0, 0);
    LZ_CHECK(program.compile_status == LZ_OK);
    LZ_CHECK(program.chunk.constant_count == 2);
    finish(&program);
}

static void test_annotations_accept_valid_programs(void) {
    check_output("let a: int = 1\nlet s: string = \"x\"\nlet f: float = 1.5\nlet b: bool = true\n"
                 "let n: nil = nil\nprintln(a, s, f, b, n)\n",
                 "1 x 1.5 true nil\n");
    check_output("let u: int | string = 5\nprintln(u)\nu = \"s\"\nprintln(u)\n", "5\ns\n");
    check_output("let m: \"r\" | \"w\" = \"r\"\nprintln(m)\nm = \"w\"\nprintln(m)\n", "r\nw\n");
    check_output("let o: int? = nil\nprintln(o)\no = 5\nprintln(o)\n", "nil\n5\n");
    check_output("let x: number = 1\nx = 2.5\nprintln(x)\n", "2.5\n");
    check_output("type Id = int | string\nlet i: Id = \"k\"\ni = 7\nprintln(i)\n", "7\n");
    check_output("let flag: true = true\nprintln(flag)\n", "true\n");
    check_output("let z: -1 | 0 | 1 = -1\nprintln(z)\n", "-1\n");
    check_output("let p: (int | string)? = nil\nprintln(p)\np = \"x\"\nprintln(p)\n", "nil\nx\n");
    check_output("let n: int?\nprintln(n)\n", "nil\n");
    check_output("{ type Small = 1 | 2\n  let s: Small = 2\n  println(s) }\n", "2\n");
    check_output("const MODE: \"r\" | \"w\" = \"r\"\nprintln(MODE)\n", "r\n");
    check_output("const K: int = 1 + 2\nprintln(K)\n", "3\n");
    check_output("let t: string = string(5) + \"!\"\nprintln(t)\n", "5!\n");
    check_output("let n: int = int(\"12\") + 1\nprintln(n)\n", "13\n");
    check_output("println(float(\"1.5\") * 2, bool(0), int(3.9))\n", "3.0 true 3\n");
    check_output("let any_value: any = 1\nany_value = \"s\"\nprintln(any_value)\n", "s\n");
}

static void test_type_names_do_not_collide_with_variables(void) {
    check_output("let type = 5\nprintln(type)\n", "5\n");
    check_output("let type = 5\ntype = 6\nprintln(type + 1)\n", "7\n");
    check_output("let int = 5\nlet x: int = 1\nprintln(int, x)\n", "5 1\n");
    check_output("let string = \"s\"\nlet y: string = \"t\"\nprintln(string, y)\n", "s t\n");
}

static void test_annotation_errors(void) {
    check_compile_error("let a: int = \"x\"\n", "type mismatch: expected int, got \"x\"");
    check_compile_error("let s: string = 1\n", "type mismatch: expected string, got 1");
    check_compile_error("let m: \"r\" | \"w\" = \"x\"\n", "expected \"r\" | \"w\", got \"x\"");
    check_compile_error("let n: int\n", "variable of type int needs an initial value");
    check_compile_error("let m: \"r\" | \"w\"\n", "needs an initial value");
    check_compile_error("let f: float = 1\n", "expected float, got 1");
    check_compile_error("let a: int = 1\na = \"s\"\n", "type mismatch: expected int");
    check_compile_error("let x: foo = 1\n", "unknown type 'foo'");
    check_compile_error("type int = string\n", "cannot redefine builtin type 'int'");
    check_compile_error("type A = int\ntype A = string\n", "type 'A' is already declared");
    check_compile_error("let x: = 1\n", "expected a type");
    check_compile_error("let x: 1.5 = 1\n", "float literal types are not supported");
    check_compile_error("let x: 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 = 1\n", "too many literal types");
    check_compile_error("const C: int = \"s\"\n", "type mismatch: expected int");
    check_compile_error("const C: string = 1 + 2\n", "type mismatch: expected string, got 3");
    check_compile_error("let x: (int = 1\n", "expected ')' after type");
    check_compile_error("let x: -a = 1\n", "expected a number after '-' in a type");
    check_compile_error("let x: 9223372036854775808 = 1\n", "integer literal out of range");
    check_compile_error("let x: never = 1\n", "type mismatch: expected never");
    check_compile_error("{ type S = int }\nlet x: S = 1\n", "unknown type 'S'");
    check_compile_error("let d: int = 7 / 2\n", "expected int, got float");
    check_compile_error("type Id = int | string\nlet i: Id = true\n", "expected int | string, got true");
}

static void test_static_operator_types(void) {
    check_compile_error("let s: string = \"a\"\nprintln(s - 1)\n",
                        "operator '-' cannot be applied to string and 1");
    check_compile_error("println(1 + \"a\")\n", "operator '+' cannot be applied to 1 and \"a\"");
    check_compile_error("println(-\"x\")\n", "operator '-' cannot be applied to \"x\"");
    check_compile_error("println(nil < 1)\n", "operator '<' cannot be applied to nil and 1");
    check_compile_error("println(true * 2)\n", "operator '*' cannot be applied");
    check_compile_error("println(1())\n", "cannot call a value of type 1");
    check_compile_error("let s: string = \"x\"\ns()\n", "cannot call a value of type string");
    check_output("let u: int | string = 1\nprintln(u + 1)\n", "2\n");
    check_output("println(\"a\" + \"b\", 1 + 2, 1 + 2.5, true and 1, nil or \"d\")\n", "ab 3 3.5 1 d\n");
    check_output("let a = 5\nprintln(a + 1, a < 9, -a)\n", "6 true -5\n");

    Program program;
    run(&program, "let a = 5\nprintln(a + \"x\")\n", 0, 0);
    LZ_CHECK(program.compile_status == LZ_OK);
    LZ_CHECK(program.run_status == LZ_ERR_RUNTIME);
    finish(&program);
}

static void test_run_time_type_checks(void) {
    check_output("let n: int = five()\nprintln(n)\n", "5\n");
    check_output("let n: int? = five()\nlet k: int = n\nprintln(k)\n", "5\n");
    check_output("let a = 5\nlet b: number = a\nprintln(b)\n", "5\n");

    Program program;
    run(&program, "let t: int = text()\nprintln(t)\n", 0, 0);
    LZ_CHECK(program.compile_status == LZ_OK);
    LZ_CHECK(program.run_status == LZ_ERR_TYPE);
    LZ_CHECK(strstr(program.vm.error, "type error: expected int, got string") != NULL);
    LZ_CHECK(program.vm.error_offset == 4);
    LZ_CHECK(program.source.text[program.vm.error_offset] == 't');
    LZ_CHECK(program.out.data == NULL);
    finish(&program);

    run(&program, "let a = 5\nlet b: string = a\n", 0, 0);
    LZ_CHECK(program.run_status == LZ_ERR_TYPE);
    LZ_CHECK(strstr(program.vm.error, "expected string, got int") != NULL);
    finish(&program);

    run(&program, "let x: int = 1\nlet y = \"s\"\nx = y\nprintln(x)\n", 0, 0);
    LZ_CHECK(program.run_status == LZ_ERR_TYPE);
    LZ_CHECK(program.source.text[program.vm.error_offset] == 'x');
    LZ_CHECK(program.out.data == NULL);
    finish(&program);

    run(&program, "let m: \"r\" | \"w\" = text()\n", 0, 0);
    LZ_CHECK(program.run_status == LZ_ERR_TYPE);
    LZ_CHECK(strstr(program.vm.error, "expected \"r\" | \"w\", got \"s\"") != NULL);
    finish(&program);

    run(&program, "{ let v: int = text() }\n", 0, 0);
    LZ_CHECK(program.run_status == LZ_ERR_TYPE);
    finish(&program);

    run(&program, "const C: int = text()\n", 0, 0);
    LZ_CHECK(program.run_status == LZ_ERR_TYPE);
    finish(&program);
}

static size_t count_checks(const char *text) {
    Program program;
    run(&program, text, 0, 0);
    LZ_CHECK(program.compile_status == LZ_OK);
    size_t checks = 0;
    for (size_t i = 0; i < program.chunk.count; i += lz_opcode_length(program.chunk.code[i])) {
        if (program.chunk.code[i] == LZ_OP_CHECK_TYPE) {
            checks++;
        }
    }
    finish(&program);
    return checks;
}

static void test_provable_types_need_no_check(void) {
    LZ_CHECK(count_checks("let x: int = 1 + 2\n") == 0);
    LZ_CHECK(count_checks("let x: float = 7 / 2\n") == 0);
    LZ_CHECK(count_checks("let x: int = 7 // 2\n") == 0);
    LZ_CHECK(count_checks("let x: number = 1 + 2.5\n") == 0);
    LZ_CHECK(count_checks("let x: string = \"a\" + \"b\"\n") == 0);
    LZ_CHECK(count_checks("let n: int = int(\"1\")\n") == 0);
    LZ_CHECK(count_checks("let s: string = string(1)\n") == 0);
    LZ_CHECK(count_checks("let x: int? = 5\n") == 0);
    LZ_CHECK(count_checks("let x: any = five()\n") == 0);
    LZ_CHECK(count_checks("let x: int = 1\nx = 2\n") == 0);
    LZ_CHECK(count_checks("let x: \"r\" | \"w\" = \"r\"\n") == 0);
    LZ_CHECK(count_checks("let x = five()\n") == 0);

    LZ_CHECK(count_checks("let x: int = five()\n") == 1);
    LZ_CHECK(count_checks("let a = 1\nlet x: int = a\n") == 1);
    LZ_CHECK(count_checks("let x: int = 1\nlet y = 2\nx = y\n") == 1);
    LZ_CHECK(count_checks("let x: int? = five()\nlet y: int = x\n") == 2);
    LZ_CHECK(count_checks("let x: int? = five()\n") == 1);
    LZ_CHECK(count_checks("let s = \"r\"\nlet m: \"r\" | \"w\" = s\n") == 1);
}

static void test_checks_show_up_in_the_disassembly(void) {
    Program program;
    run(&program, "let x: int? = 5\n", 0, 0);
    LZ_CHECK(program.compile_status == LZ_OK);

    LzBuffer listing = {0};
    LzWriter writer = lz_writer_from_buffer(&listing);
    lz_disassemble_chunk(&program.chunk, "t", &program.source, &writer);
    LZ_CHECK(listing.data != NULL && strstr(listing.data, "OP_CHECK_TYPE") == NULL);
    lz_buffer_free(&listing);
    finish(&program);

    run(&program, "let x: int? = five()\n", 0, 0);
    writer = lz_writer_from_buffer(&listing);
    lz_disassemble_chunk(&program.chunk, "t", &program.source, &writer);
    LZ_CHECK(listing.data != NULL && strstr(listing.data, "'int | nil'") != NULL);
    lz_buffer_free(&listing);
    finish(&program);

    run(&program, "let x: int = five()\n", 0, 0);
    writer = lz_writer_from_buffer(&listing);
    lz_disassemble_chunk(&program.chunk, "t", &program.source, &writer);
    LZ_CHECK(listing.data != NULL && strstr(listing.data, "OP_CHECK_TYPE") != NULL);
    LZ_CHECK(strstr(listing.data, "'int'") != NULL);
    lz_buffer_free(&listing);
    finish(&program);
}

int main(void) {
    test_annotations_accept_valid_programs();
    test_type_names_do_not_collide_with_variables();
    test_annotation_errors();
    test_static_operator_types();
    test_run_time_type_checks();
    test_provable_types_need_no_check();
    test_checks_show_up_in_the_disassembly();
    test_expressions();
    test_short_circuit();
    test_globals_and_assignment();
    test_blocks_and_locals();
    test_control_flow();
    test_fizzbuzz();
    test_constants();
    test_syntax_forms();
    test_compile_errors();
    test_sequential_execution();
    test_recovery_reports_several_errors();
    test_many_errors_are_capped();
    test_limits_in_the_compiler();
    test_runtime_behaviour();
    test_const_folding_leaves_no_code();
    test_long_boolean_chains();
    test_long_else_if_chains();
    test_int64_min_literal();
    test_constants_are_shared();
    test_many_references_fit_in_one_chunk();
    test_folded_consts_with_a_shared_table();
    test_constant_table_has_no_duplicates();
    test_fuel_covers_large_strings();
    test_fuel_covers_every_large_string_operation();
    test_else_if_branches_report_their_own_line();
    test_heap_limit_status_while_compiling();
    return LZ_TEST_RESULT();
}
