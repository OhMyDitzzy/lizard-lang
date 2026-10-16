#include <stdint.h>
#include <string.h>

#include "lizard/builtin.h"
#include "lizard/compiler.h"
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

static void define_five(LzVM *vm) {
    LzString *name = lz_vm_intern(vm, "five", 4);
    LzNative *native = lz_native_new(&vm->heap, name, five, 0, 0);
    (void)lz_vm_define_global(vm, name, lz_value_object(&native->base));
    lz_object_release(&vm->heap, &native->base);
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
    LZ_CHECK(program.vm.globals.count == 1);
    finish(&program);

    run(&program, "const G = \"he\" + \"llo\"\nprintln(G, G + \"!\")\n", 0, 0);
    LZ_CHECK(program.out.data != NULL && strcmp(program.out.data, "hello hello!\n") == 0);
    LZ_CHECK(program.vm.globals.count == 1);
    finish(&program);

    run(&program, "const C = five() + 1\nprintln(C)\n", 0, 0);
    LZ_CHECK(program.compile_status == LZ_OK && program.run_status == LZ_OK);
    LZ_CHECK(program.out.data != NULL && strcmp(program.out.data, "6\n") == 0);
    LZ_CHECK(program.vm.globals.count == 2);
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
    check_compile_error("println(y)\n", "undefined variable 'y'");
    check_compile_error("let x = 1\nlet x = 2\n", "already declared");
    check_compile_error("{ let a = 1\n let a = 2 }\n", "already declared");
    check_compile_error("const X = 1\nX = 2\n", "cannot assign to constant 'X'");
    check_compile_error("const X = five()\nX = 2\n", "cannot assign to constant 'X'");
    check_compile_error("{ const X = 1\n X = 2 }\n", "cannot assign to constant 'X'");
    check_compile_error("println = 1\n", "cannot assign to builtin 'println'");
    check_compile_error("y = 1\n", "undefined variable 'y'");
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
    check_compile_error("const Z = \"a\" + 1\n", "two numbers or two strings");
    check_compile_error("{ let a = a }\n", "undefined variable 'a'");
}

static void test_recovery_reports_several_errors(void) {
    Program program;
    run(&program, "let x = 1\nlet x = 2\nprintln(y)\nconst Z = 1 / 0\nx = = 3\nprintln(x)\n", 0, 0);
    LZ_CHECK(program.compile_status == LZ_ERR_COMPILE);
    LZ_CHECK(program.diagnostics.count == 4);
    LZ_CHECK(strstr(program.diagnostics.items[0].message, "already declared") != NULL);
    LZ_CHECK(strstr(program.diagnostics.items[1].message, "undefined variable 'y'") != NULL);
    LZ_CHECK(strstr(program.diagnostics.items[2].message, "division by zero") != NULL);
    LZ_CHECK(strstr(program.diagnostics.items[3].message, "expected expression") != NULL);

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

int main(void) {
    test_expressions();
    test_short_circuit();
    test_globals_and_assignment();
    test_blocks_and_locals();
    test_control_flow();
    test_fizzbuzz();
    test_constants();
    test_syntax_forms();
    test_compile_errors();
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
    test_heap_limit_status_while_compiling();
    return LZ_TEST_RESULT();
}
