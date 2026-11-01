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

static void test_statement_values(void) {
    const char *kind = "fn kind(v) {\n"
                       "  switch v {\n"
                       "    case 1 { return \"one\" }\n"
                       "    case 2, 3 { return \"two-three\" }\n"
                       "    case 4..=9 { return \"small\" }\n"
                       "    case \"a\" { return \"letter\" }\n"
                       "    case nil { return \"nil\" }\n"
                       "    case true { return \"yes\" }\n"
                       "    case [1, 2] { return \"array\" }\n"
                       "    case {\"k\": 1} { return \"dict\" }\n"
                       "    else { return \"other\" }\n"
                       "  }\n}\n";
    char program[1024];
    const char *cases[][2] = {
        {"1", "one"},       {"2", "two-three"}, {"3", "two-three"}, {"4", "small"},
        {"9", "small"},     {"10", "other"},    {"0", "other"},     {"\"a\"", "letter"},
        {"\"b\"", "other"}, {"nil", "nil"},     {"true", "yes"},    {"false", "other"},
        {"[1, 2]", "array"}, {"[2, 1]", "other"}, {"{\"k\": 1}", "dict"}, {"{\"k\": 2}", "other"},
        {"1.0", "one"},     {"2.5", "other"},   {"4.5", "small"},   {"9.5", "other"},
        {"3.0", "two-three"},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        (void)snprintf(program, sizeof program, "%sprintln(kind(%s))\n", kind, cases[i][0]);
        char expected[64];
        (void)snprintf(expected, sizeof expected, "%s\n", cases[i][1]);
        check_output(program, expected);
    }
    /* `..` leaves its end out, `..=` keeps it. */
    check_output("for v in [3, 4, 8, 9] { switch v { case 4..9 { print(\"in \") } else { print(\"out \") } } }\n",
                 "out in in out ");
    check_output("for v in [3, 4, 9, 10] { switch v { case 4..=9 { print(\"in \") } else { print(\"out \") } } }\n",
                 "out in in out ");
    /* No match and no else: nothing happens. */
    check_output("switch 5 { case 1 { println(\"a\") } case 2 { println(\"b\") } }\nprintln(\"done\")\n",
                 "done\n");
    check_output("switch 5 { }\nprintln(\"empty\")\n", "empty\n");
    /* Both body forms in one statement. */
    check_output("switch 2 {\n case 1 => println(\"a\")\n case 2 => println(\"b\")\n else { println(\"c\") }\n}\n",
                 "b\n");
    check_output("switch 3 { case 1 => println(\"a\") else => println(\"z\") }\n", "z\n");
    /* A name in a case is read, not declared. */
    check_output("let limit = 10\nswitch 10 { case limit { println(\"limit\") } else { println(\"no\") } }\n",
                 "limit\n");
    /* A range with a non-number subject is no match, at run time. */
    check_output("fn f(v) { switch v { case 1..5 { return \"in\" } else { return \"out\" } } }\n"
                 "println(f(\"x\"), f(nil), f([1]), f(3), f(true))\n",
                 "out out out in out\n");
    check_output("let hi = 4\nlet lo = 2\nswitch 3 { case lo..hi { println(\"in\") } else { println(\"out\") } }\n",
                 "in\n");
}

static void test_evaluation_order(void) {
    check_output("let n = 0\nfn subject() { n += 1\n return 2 }\n"
                 "switch subject() { case 1 { } case 2 { } case 3 { } else { } }\nprintln(n)\n",
                 "1\n");
    /* Items are tried left to right, arms top to bottom, and only until one matches. */
    check_output("let log = []\nfn t(v) { push(log, v)\n return v }\n"
                 "switch 3 { case t(1), t(2) { } case t(3), t(4), t(5) { } case t(6) { } }\nprintln(log)\n",
                 "[1, 2, 3]\n");
    check_output("let log = []\nfn t(v) { push(log, v)\n return v }\n"
                 "let r = switch 1 { case t(1) => \"a\" case t(2) => \"b\" else => \"c\" }\nprintln(r, log)\n",
                 "a [1]\n");
    /* The bounds of a range are evaluated once, when their case is tried. */
    check_output("let n = 0\nfn t(v) { n += 1\n return v }\n"
                 "switch 5 { case t(1)..t(9) { println(n) } }\n", "2\n");
    check_output("let n = 0\nfn t(v) { n += 1\n return v }\n"
                 "switch 50 { case t(1)..t(9) { } case t(40)..=t(60) { println(n) } }\n", "4\n");
    /* The first match wins. */
    check_output("switch 3 { case 1..5 { println(\"range\") } case 3 { println(\"three\") } }\n", "range\n");
}

static void test_expressions(void) {
    check_output("let d = 6\nlet name = switch d {\n  case 1 => \"Mon\"\n  case 6, 7 => \"Weekend\"\n  else => \"Weekday\"\n}\nprintln(name)\n",
                 "Weekend\n");
    check_output("fn f(x) { return switch x { case 1 => \"a\" else => \"b\" } }\nprintln(f(1), f(2))\n", "a b\n");
    check_output("println(switch 2 { case 1 => 10 case 2 => 20 else => 30 })\n", "20\n");
    check_output("let f = fn(x) => switch x { case 0 => \"z\" case 1..=9 => \"d\" else => \"n\" }\nprintln(f(0), f(5), f(50))\n",
                 "z d n\n");
    check_output("println(1 + switch 1 { case 1 => 10 else => 20 } * 2)\n", "21\n");
    check_output("println([1, 2, 3].map(fn(x) => switch x { case 2 => \"two\" else => \"no\" }))\n",
                 "[\"no\", \"two\", \"no\"]\n");
    /* Nested, in an `if` branch and as a subject. */
    check_output("let a = 1\nlet b = 2\nprintln(switch a { case 1 => switch b { case 2 => \"x\" else => \"y\" } else => \"z\" })\n",
                 "x\n");
    check_output("let c = true\nprintln(if c { switch 3 { case 3 => \"yes\" else => \"no\" } } else { \"other\" })\n",
                 "yes\n");
    check_output("println(switch switch 1 { case 1 => 2 else => 3 } { case 2 => \"two\" else => \"?\" })\n", "two\n");
    /* One line, multi-line, and in a template. */
    check_output("let x = 1\nprintln(switch x { case 1 => \"a\" else => \"b\" })\n", "a\n");
    check_output("let x = 2\nprintln($\"{switch x { case 1 => \"a\" else => \"b\" }}\")\n", "b\n");
    check_output("let x = 2\nprintln($\"v={switch x { case 1 => \"a\" case 2, 3 => \"c\" else => \"b\" }}!\")\n", "v=c!\n");
    check_output("let x = 1\nlet s = switch x {\n\n  case 1 =>\n    \"a\"\n\n  else =>\n    \"b\"\n\n}\nprintln(s)\n", "a\n");
    /* A union of the arms' types. */
    check_output("let x: int | string = switch 1 { case 1 => 5 else => \"s\" }\nprintln(x)\n", "5\n");
    check_compile_error("let x: bool = switch 1 { case 1 => 5 else => \"s\" }\n", "type mismatch", 1, 15);
    check_output("let r = switch [1] { case [1] => \"s\" else => \"n\" }\nprintln(r)\n", "s\n");
    check_output("let r = switch 1 { case 1 => {\"a\": 1} else => {} }\nprintln(r)\n", "{\"a\": 1}\n");
}

static void test_subjectless(void) {
    const char *body = "fn f(n) {\n  switch {\n    case n < 0 { return \"neg\" }\n"
                       "    case n equal 0 { return \"zero\" }\n    case n > 100, n < -100 { return \"big\" }\n"
                       "    else { return \"pos\" }\n  }\n}\n";
    char program[512];
    (void)snprintf(program, sizeof program, "%sprintln(f(-5), f(0), f(7), f(500))\n", body);
    check_output(program, "neg zero pos big\n");
    check_output("switch { case 1 > 2 { println(\"a\") } case 2 > 1 { println(\"b\") } else { println(\"c\") } }\n", "b\n");
    check_output("switch { case false { println(\"a\") } }\nprintln(\"none\")\n", "none\n");
    check_output("let n = 0\nfn t(v) { n += 1\n return v }\nswitch { case t(false), t(true), t(true) { println(n) } }\n", "2\n");
    check_output("let x = 5\nprintln(switch { case x < 3 => \"low\" case x < 10 => \"mid\" else => \"high\" })\n", "mid\n");
    check_compile_error("switch { case 1..3 { } }\n", "a range cannot be used in a 'switch' without a subject", 1, 15);
    /* A dict literal as the subject needs brackets. */
    check_output("switch ({\"a\": 1}) { case {\"a\": 1} { println(\"same\") } }\n", "same\n");
}

static void test_jumps(void) {
    check_output("let t = 0\nfor i in 0..6 { switch i { case 1 { continue } case 4 { break } else { t += i } } }\nprintln(t)\n", "5\n");
    check_output("let t = 0\nlet i = 0\nwhile i < 10 { i += 1\n switch i { case 3 { continue } case 6 { break } else { t += i } } }\nprintln(t)\n",
                 "12\n");
    check_output("let t = 0\nfor let i = 0; i < 10; i += 1 { switch i { case 2 { continue } case 5 { break } else { t += i } } }\nprintln(t)\n",
                 "8\n");
    check_output("let t = 0\nfor x in [1, 2, 3, 4] { switch x { case 2 { let a = x * 10\n t += a\n continue } else { t += 1 } } }\nprintln(t)\n",
                 "23\n");
    /* Arm locals and the subject are dropped on the way out; the loop's variables are intact. */
    check_output("let out = []\nfor i in 0..4 {\n let keep = i * 2\n switch i {\n  case 1 { let a = 1\n let b = 2\n continue }\n"
                 "  case 3 { let c = 3\n break }\n  else { push(out, keep) }\n }\n push(out, -1)\n}\nprintln(out)\n",
                 "[0, -1, 4, -1]\n");
    /* Nested loops: break leaves the loop around the switch, not an outer one. */
    check_output("let n = 0\nfor i in 0..3 { for j in 0..5 { switch j { case 2 { break } else { n += 1 } } } }\nprintln(n)\n", "6\n");
    check_output("let n = 0\nfor i in 0..3 { switch i { case 1 { for j in 0..4 { switch j { case 2 { continue } case 3 { break } else { n += 1 } } } } else { n += 10 } } }\nprintln(n)\n",
                 "22\n");
    /* A switch inside a switch. */
    check_output("let n = 0\nfor i in 0..4 { switch i { case 1, 2 { switch i { case 2 { continue } else { n += 1 } }\n n += 100 } else { n += 1000 } } }\nprintln(n)\n",
                 "2101\n");
    /* A closure over an arm variable and over the loop variable. */
    check_output("let fs = []\nfor i in 0..3 { switch i { case 0, 1 { let k = i * 10\n push(fs, fn() => k + i)\n if k equal 10 { continue }\n push(fs, fn() => -1) }\n"
                 " case 2 { let z = 5\n push(fs, fn() => z)\n break } } }\nfor f in fs { print(f(), \"\") }\nprintln()\n",
                 "0 -1 11 5 \n");
    /* return from inside, with a switch expression as the value. */
    check_output("fn g(x) { for i in 0..3 { switch x { case 1 { let a = 1\n return switch i { case 0 => a else => 9 } } else { let b = i } } }\n return \"end\" }\nprintln(g(1), g(2))\n",
                 "1 end\n");
    check_output("fn g(x) { while true { switch x { case 1 { return \"one\" } else { return \"other\" } } } }\nprintln(g(1), g(2))\n", "one other\n");
    /* Many rounds, and the stack is empty at the end (finish() checks that). */
    check_output("let c = 0\nlet i = 0\nwhile i < 100000 { switch i % 3 { case 0 { c += 1 } case 1 { i += 0 } else { c += 2 } }\n"
                 " c += switch i % 2 { case 0 => 1 else => 0 }\n i += 1 }\nprintln(c)\n",
                 "150000\n");
    check_output("let c = 0\nfor i in 0..100000 { switch i % 4 { case 0 { continue } case 3 { c += 1\n continue } else { c += 2 } } }\nprintln(c)\n",
                 "125000\n");
    /* break and continue outside a loop are still errors, with a switch around them. */
    check_compile_error("switch 1 { case 1 { break } }\n", "'break' outside of a loop", 1, 21);
    check_compile_error("fn f() { for i in 0..3 { switch i { case 1 { fn g() { break } } } } }\n",
                        "'break' outside of a loop (a function body is not part of the loop)", 1, 55);
}

static void test_contexts(void) {
    check_output("fn f(x) { switch x { case 1 { return \"a\" } else { return \"b\" } } }\nprintln(f(1), f(2))\n", "a b\n");
    check_output("let g = fn(x) { switch x { case 1 { return \"a\" } else { return \"b\" } } }\nprintln(g(1), g(2))\n", "a b\n");
    check_output("println([1, 2, 3].map(fn(x) { switch x { case 2 { return \"two\" } else { return \"no\" } } }))\n", "[\"no\", \"two\", \"no\"]\n");
    check_output("println([1, 2, 3, 4].filter(fn(x) => switch x { case 2, 4 => true else => false }))\n", "[2, 4]\n");
    check_output("fn f(a) { return fn(x) { switch x { case a { return \"same\" } else { return \"diff\" } } } }\nprintln(f(1)(1), f(1)(2))\n",
                 "same diff\n");
    /* An `if` in an arm, and the `else` of an arm next to the `else` of an `if`. */
    check_output("switch 1 { case 1 { if false { println(\"x\") } else { println(\"if-else\") } } else { println(\"arm-else\") } }\n",
                 "if-else\n");
    check_output("switch 2 { case 1 { if true { println(\"x\") } } else { if false { println(\"y\") } else { println(\"inner\") } } }\n",
                 "inner\n");
    check_output("switch 1 { case 1 => println(if true { \"a\" } else { \"b\" }) else => println(\"c\") }\n", "a\n");
    check_output("let x = 1\nswitch x {\n case 1 {\n  switch x + 1 {\n   case 2 { println(\"nested\") }\n  }\n }\n}\n", "nested\n");
}

static void test_terminated(void) {
    /* Every arm returns and there is an else: nothing is needed after. */
    check_output("fn f(x) { switch x { case 1 { return \"a\" } case 2 { return \"b\" } else { return \"c\" } } }\nprintln(f(1), f(2), f(3))\n",
                 "a b c\n");
    check_output("fn f(x) { switch x { case 1 { return \"a\" } else { return \"c\" } }\n println(\"never\") }\nprintln(f(1))\n", "a\n");
    /* Without an else, or with an arm that falls through, the function can reach its end. */
    check_output("fn f(x) { switch x { case 1 { return \"a\" } }\n return \"end\" }\nprintln(f(1), f(2))\n", "a end\n");
    check_output("fn f(x) { switch x { case 1 { return \"a\" } else { println(\"fall\") } }\n return \"end\" }\nprintln(f(2))\n", "fall\nend\n");
    check_output("fn f(x) { switch x { case 1 => 5 else => 6 }\n return \"end\" }\nprintln(f(2))\n", "end\n");
    check_output("fn f() { while true { switch 1 { case 1 { break } else { return 1 } } }\n return \"out\" }\nprintln(f())\n", "out\n");
}

static void test_errors(void) {
    check_compile_error("let x = switch 1 { case 1 { 2 } else => 3 }\n", "an arm of a 'switch' expression must use '=>'", 1, 27);
    check_compile_error("let x = switch 1 { case 1 => 2 }\n", "a 'switch' expression needs an 'else'", 1, 9);
    check_compile_error("switch 1 { else { } case 1 { } }\n", "'else' must be the last arm", 1, 21);
    check_compile_error("let x = switch 1 { else => 1 case 2 => 2 }\n", "'else' must be the last arm", 1, 30);
    check_compile_error("switch 1 { case 1 { } case 1 { } }\n", "duplicate case 1", 1, 28);
    check_compile_error("switch 1 { case 1, 2 { } case 3, 2 { } }\n", "duplicate case 2", 1, 34);
    check_compile_error("switch \"a\" { case \"a\" { } case \"b\" { } case \"a\" { } }\n", "duplicate case \"a\"", 1, 45);
    check_compile_error("switch true { case true { } case true { } }\n", "duplicate case true", 1, 34);
    check_compile_error("switch nil { case nil { } case nil { } }\n", "duplicate case nil", 1, 32);
    check_compile_error("switch 1 { case -1 { } case -1 { } }\n", "duplicate case -1", 1, 29);
    check_output("switch 1 { case 1 { println(\"a\") } case -1 { } case 2 { } }\n", "a\n");
    check_compile_error("switch \"a\" { case 1 { } }\n", "this case can never match: the subject is string, but the case is int", 1, 19);
    check_compile_error("switch 1 { case \"a\" { } }\n", "this case can never match: the subject is int, but the case is string", 1, 17);
    check_compile_error("switch 1 { case nil { } }\n", "this case can never match: the subject is int, but the case is nil", 1, 17);
    check_compile_error("switch \"a\" { case 1..3 { } }\n", "this case can never match: the subject is string, but the case is int", 1, 19);
    check_output("switch 1.5 { case 1 { println(\"int\") } case 1.5 { println(\"float\") } }\n", "float\n");
    check_output("switch 1 { case 1.0 { println(\"float\") } }\n", "float\n");
    check_output("fn f(x) { switch x { case \"a\" { return 1 } case 2 { return 2 } } }\nprintln(f(2))\n", "2\n");
    check_compile_error("switch 1 { case 1..3 by 2 { } }\n", "a range with 'by' cannot be used in a 'case'", 1, 22);
    check_compile_error("switch { case 1..3 { } }\n", "a range cannot be used in a 'switch' without a subject", 1, 15);
    check_compile_error("switch 1 { case 1 x }\n", "expected '{' or '=>' after the case", 1, 19);
    check_compile_error("switch 1 { foo }\n", "expected 'case' or 'else' in a 'switch'", 1, 12);
    check_compile_error("switch 1 { case }\n", "expected an expression after 'case'", 1, 17);
    check_compile_error("switch 1 { case 1, }\n", "expected an expression after 'case'", 1, 20);
    check_compile_error("switch 1 { case 1 { }\n", "expected '}' at the end of the 'switch'", 2, 1);
    check_compile_error("switch 1\n", "expected '{' after the 'switch' subject", 1, 9);
    /* Each mistake is one error: nothing follows it. */
    {
        Program program;
        run(&program, "switch 1 { case 1 { } case 1 { } }\nprintln(\"x\")\n", 0);
        LZ_CHECK(program.compile_status != LZ_OK);
        LZ_CHECK(program.diagnostics.count == 1);
        finish(&program);
        run(&program, "let x = switch 1 { case 1 { 2 } else => 3 }\nprintln(x)\n", 0);
        LZ_CHECK(program.diagnostics.count == 1);
        finish(&program);
        run(&program, "switch 1 { case 1 x }\nprintln(1)\n", 0);
        LZ_CHECK(program.diagnostics.count == 1);
        finish(&program);
        run(&program, "switch 1 { foo bar }\nprintln(1)\n", 0);
        LZ_CHECK(program.diagnostics.count == 1);
        finish(&program);
    }
}

static void test_names(void) {
    check_compile_error("let switch = 1\n", "expected variable name after 'let'", 1, 5);
    check_compile_error("let case = 1\n", "expected variable name after 'let'", 1, 5);
    check_output("let switcher = 1\nlet cases = 2\nlet casey = 3\nlet myswitch = 4\nprintln(switcher + cases + casey + myswitch)\n", "10\n");
    check_output("fn switch_it(case_no) { return case_no }\nprintln(switch_it(5))\n", "5\n");
}

static void test_fuel_and_values(void) {
    check_runs_out_of_fuel("let i = 0\nwhile true { switch i { case 0 { i = 1 } case 1 { i = 0 } } }\n", 10000);
    check_run_error("fn f(x) { return switch x { case 1..3 => 1 else => 2 } }\nprintln(f(1))\nprintln(1 / 0)\n", "division");
    check_output("let x = 0\nswitch x { case 0 { x = 5 } }\nprintln(x)\n", "5\n");
}

int main(void) {
    test_statement_values();
    test_evaluation_order();
    test_expressions();
    test_subjectless();
    test_jumps();
    test_contexts();
    test_terminated();
    test_errors();
    test_names();
    test_fuel_and_values();
    return LZ_TEST_RESULT();
}
