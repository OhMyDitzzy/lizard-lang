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
static void run_limited(Program *program, const char *text, uint64_t fuel, size_t heap_limit) {
    memset(program, 0, sizeof *program);
    lz_vm_init(&program->vm, lz_writer_from_buffer(&program->out),
               lz_writer_from_buffer(&program->err));
    LZ_CHECK(lz_builtin_install(&program->vm) == LZ_OK);
    if (heap_limit != 0) {
        lz_vm_set_heap_limit(&program->vm, heap_limit);
    }

    lz_source_from_string(&program->source, "t.lz", text);
    program->vm.trace_source = &program->source;
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

static void run(Program *program, const char *text, uint64_t fuel) {
    run_limited(program, text, fuel, 0);
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

/* A failed run (the status, and the message in vm->error). */
static void check_fails(const char *text, uint64_t fuel, size_t heap_limit, LzStatus status,
                        const char *message, const char *expected_output) {
    Program program;
    run_limited(&program, text, fuel, heap_limit);
    LZ_CHECK(program.compile_status == LZ_OK);
    bool ok = program.run_status == status && strstr(program.vm.error, message) != NULL;
    const char *actual = program.out.data != NULL ? program.out.data : "";
    if (!ok || strcmp(actual, expected_output) != 0) {
        fprintf(stderr, "program:\n%s\nexpected status %d [%s] and output [%s]; got %d [%s] [%s]\n",
                text, (int)status, message, expected_output, (int)program.run_status,
                program.vm.error, actual);
        ok = false;
    }
    LZ_CHECK(ok);
    finish(&program);
}


/* Declared kinds of error: `error Name : Parent`, cause, data, hoisting (tests/test_error_types.c). */

static void test_declarations(void) {
    check_output("error NotFound : IOError\n"
                 "try { throw NotFound(\"gone\") } catch NotFound as e { println(e.kind, e.message) }\n",
                 "NotFound gone\n");
    /* Without a parent, Error. */
    check_output("error AppError\n"
                 "try { throw AppError(\"x\") } catch Error as e { println(e.kind) }\n", "AppError\n");
    check_output("error AppError\n"
                 "try { throw AppError(\"x\") } catch RuntimeError as e { println(1) } catch e { println(2) }\n",
                 "2\n");
    /* Three levels, caught by the kind, the parent, an ancestor and a built-in ancestor. */
    const char *chain = "error A : IOError\nerror B : A\nerror C : B\n";
    const char *throws[] = {"C", "B", "A", "IOError", "Error"};
    for (size_t i = 0; i < sizeof throws / sizeof throws[0]; i++) {
        char text[300];
        (void)snprintf(text, sizeof text,
                       "%stry { throw C(\"deep\") } catch %s as e { println(e.kind, e.message) }\n",
                       chain, throws[i]);
        check_output(text, "C deep\n");
    }
    check_output("error A : IOError\nerror B : A\n"
                 "try { throw A(\"a\") } catch B as e { println(\"B\") } catch A as e { println(\"A\") }\n",
                 "A\n");
    check_output("error A\nerror B\n"
                 "try { throw B(\"b\") } catch A as _ { println(\"A\") } catch e { println(e.kind) }\n", "B\n");
    check_compile_error("error A\nerror B : A\n"
                        "try { throw B(\"b\") } catch A as _ { println(1) } catch B as _ { println(2) }\n",
                        "unreachable catch: 'B' is already caught by 'A' above", 3, 56);
    check_compile_error("error A : IOError\n"
                        "try { throw A(\"b\") } catch IOError as _ { println(1) } catch A as _ { println(2) }\n",
                        "unreachable catch: 'A' is already caught by 'IOError' above", 2, 62);
}

static void test_hoisting(void) {
    /* Used above the declaration: caught, called, thrown. */
    check_output("try { throw NotFound(\"early\") } catch NotFound as e { println(e.kind, e.message) }\n"
                 "error NotFound : IOError\n", "NotFound early\n");
    check_output("fn f() { throw Late(\"in f\") }\n"
                 "try { f() } catch Late as e { println(e.message) }\n"
                 "error Late : ValueError\n"
                 "try { f() } catch ValueError as e { println(e.kind) }\n", "in f\nLate\n");
    /* The kind exists when the program starts, not when the declaration runs. */
    check_output("let made = Early(\"m\")\nprintln(made.kind)\nerror Early\n", "Early\n");
    /* A parent has to come first in the file. */
    check_compile_error("error Child : Parent\nerror Parent\n", "unknown error type 'Parent'", 1, 15);
    /* The VM knows the kinds right after the compile. */
    Program program;
    run(&program, "error X : IOError\nerror Y : X\n", 0);
    LZ_CHECK(program.compile_status == LZ_OK);
    uint8_t x, y;
    LZ_CHECK(lz_vm_error_kind_from_name(&program.vm, "X", 1, &x));
    LZ_CHECK(lz_vm_error_kind_from_name(&program.vm, "Y", 1, &y));
    LZ_CHECK(lz_vm_error_kind_is(&program.vm, y, x));
    LZ_CHECK(lz_vm_error_kind_is(&program.vm, y, LZ_ERROR_IO));
    LZ_CHECK(lz_vm_error_kind_is(&program.vm, y, LZ_ERROR_ERROR));
    LZ_CHECK(!lz_vm_error_kind_is(&program.vm, x, y));
    LZ_CHECK(!lz_vm_error_kind_is(&program.vm, y, LZ_ERROR_RUNTIME));
    finish(&program);
}

static void test_declaration_rules(void) {
    check_compile_error("fn f() {\n    error Inner\n}\n",
                        "an 'error' declaration is only allowed at the top level", 2, 5);
    check_compile_error("{\n    error Inner\n}\n",
                        "an 'error' declaration is only allowed at the top level", 2, 5);
    check_compile_error("for i in [1] {\n    error Inner : IOError\n}\n",
                        "an 'error' declaration is only allowed at the top level", 2, 5);
    check_compile_error("let i = 0\nwhile i < 1 {\n    error Inner\n    i += 1\n}\n",
                        "an 'error' declaration is only allowed at the top level", 3, 5);
    check_compile_error("error A\nerror A\n", "duplicate error type 'A'", 2, 7);
    check_compile_error("error IOError\n", "duplicate error type 'IOError'", 1, 7);
    check_compile_error("error Error\n", "duplicate error type 'Error'", 1, 7);
    check_compile_error("error A : Nope\n", "unknown error type 'Nope'", 1, 11);
    check_compile_error("error A : A\n", "unknown error type 'A'", 1, 11);
    check_compile_error("let X = 1\nerror X\n", "'X' is already used by a variable or function", 2, 7);
    check_compile_error("error X\nlet X = 1\n", "'X' is already used by a variable or function", 1, 7);
    check_compile_error("fn X() { return 1 }\nerror X\n",
                        "'X' is already used by a variable or function", 2, 7);
    check_compile_error("error print\n", "'print' is already used by a variable or function", 1, 7);
    check_compile_error("error A :\n", "expected the name of a parent error type", 1, 10);
    check_compile_error("error A : IOError : Error\n", "expected", 1, 19);

    /* `error` is a name everywhere else. */
    check_output("let error = 5\nprintln(error + 1)\nerror = 7\nprintln(error)\n", "6\n7\n");
    check_output("let error = \"e\"\nfn f(error) { return error }\nprintln(f(error))\n", "e\n");
    check_output("let error = [1, 2]\nprintln(error[1])\n", "2\n");
    check_output("let error = 1\nerror += 2\nprintln(error)\n", "3\n");
    /* A line that starts with `error` and a name is the declaration. */
    check_output("let error = 1\nerror Plain\nprintln(error, Plain(\"p\").kind)\n", "1 Plain\n");
    /* In the middle of a line it is the name. */
    check_output("let error = 3\nlet x = error + error\nprintln(x)\n", "6\n");
    check_output("let error = 3\nlet ok = true; error = 4\nprintln(error)\n", "4\n");
    /* The limit of kinds, with a message that says so. */
    {
        size_t cap = 256 * 24 + 64;
        char *text = malloc(cap);
        size_t used = 0;
        for (int i = 0; i < 260; i++) {
            used += (size_t)snprintf(text + used, cap - used, "error K%d\n", i);
        }
        Program program;
        run(&program, text, 0);
        LZ_CHECK(program.compile_status == LZ_ERR_COMPILE);
        LZ_CHECK(program.diagnostics.count > 0 &&
                 strstr(program.diagnostics.items[0].message, "too many error types (limit 256)") != NULL);
        finish(&program);
        free(text);
    }
}

static void test_constructors(void) {
    check_output("error E\nlet a = E(\"m\")\nprintln(a.message, a.cause, a.data)\n", "m nil nil\n");
    check_output("error E\nlet inner = E(\"in\")\nlet a = E(\"out\", cause: inner, data: {\"n\": 3})\n"
                 "println(a.cause.message, a.data[\"n\"])\n", "in 3\n");
    check_output("error E\nlet a = E(\"m\", data: [1, 2, 3])\nprintln(a.data[2], len(a.data))\n", "3 3\n");
    check_output("error E\nlet a = E(\"m\", data: nil, cause: nil)\nprintln(a.cause, a.data)\n", "nil nil\n");
    check_output("error E\nlet a = E(\"m\", cause = E(\"x\"))\nprintln(a.cause.message)\n", "x\n");
    check_output("error E\nlet a = E(\"m\", data: 1, cause: nil)\nprintln(a.data)\n", "1\n");
    /* The built-in kinds take them too. */
    check_output("let a = ValueError(\"v\", cause: IOError(\"io\"), data: \"d\")\n"
                 "println(a.kind, a.cause.kind, a.data)\n", "ValueError IOError d\n");
    check_output("let a = Error(\"v\", data: 4)\nprintln(a.data)\n", "4\n");
    /* A function value takes the same: through a variable, through map. */
    check_output("error E\nlet f = E\nprintln(f(\"viaf\").kind)\n", "E\n");
    check_output("error E\nprintln(len(map([\"a\", \"b\"], E)))\n", "2\n");
    check_output("error E\nprintln(map([\"a\", \"b\"], E)[1].message)\n", "b\n");
    /* Wrong arguments. */
    check_compile_error("error E\nE(\"m\", color: 1)\n", "unexpected named argument 'color'", 2, 8);
    check_run_error("error E\nfn go() { return E(\"m\", 1) }\nlet f = [go][0]\nf()\n", "wrong number of arguments");
    check_run_error("error E\nlet f = E\nf(\"m\", 1)\n", "wrong number of arguments");
    check_run_error("error E\nE()\n", "wrong number of arguments");
    check_run_error("error E\nE(\"a\", \"b\", \"c\")\n", "wrong number of arguments");
    check_run_error("error E\nE(\"m\", cause: 5)\n", "the cause of an error must be an error or nil");
    check_run_error("error E\nE(\"m\", cause: \"s\")\n", "the cause of an error must be an error or nil");
    check_run_error("error E\nE(5)\n", "the message of an error must be a string");
    check_run_error("ValueError(\"m\", cause: [1])\n", "the cause of an error must be an error or nil");
    check_run_error("Error(\"a\", \"b\")\n", "wrong number of arguments");
    check_output("error E\ntry { E(\"m\", cause: 5) } catch TypeError as t { println(t.kind) }\n", "TypeError\n");
    check_output("error E\ntry { E(\"a\", \"b\") } catch ArityError as t { println(t.kind) }\n", "ArityError\n");
    /* `cause` and `data` are read only. */
    /* The kind is declared at run time, so `a` has no known type: the check is made when it runs. */
    check_run_error("error E\nlet a = E(\"m\")\na.cause = a\n", "cannot assign to field 'cause' of error");
}

static void test_fields_and_chain(void) {
    check_output("error A\nerror B\nerror C\n"
                 "let c = C(\"c\")\nlet b = B(\"b\", cause: c)\nlet a = A(\"a\", cause: b)\n"
                 "println(a.cause.message, a.cause.cause.message, a.cause.cause.cause)\n", "b c nil\n");
    check_output("error A\nlet e = A(\"m\")\nprintln(e.cause == nil, e.data == nil)\n", "true true\n");
    /* Through a catch. */
    check_output("error ParseError : ValueError\n"
                 "try {\n try { throw IOError(\"disk\") } catch e { throw ParseError(\"line 3\", cause: e, data: {\"line\": 3}) }\n"
                 "} catch ParseError as p { println(p.message, p.cause.kind, p.cause.message, p.data[\"line\"]) }\n",
                 "line 3 IOError disk 3\n");
    /* Rethrowing keeps the cause. */
    check_output("error A\ntry { try { throw A(\"x\", cause: IOError(\"c\")) } catch e { throw e } }"
                 " catch e { println(e.cause.message) }\n", "c\n");
    /* A long chain walked in a loop. */
    check_output("error L\nlet e = L(\"0\")\nfor i in range(1, 50) { e = L(\"n\", cause: e) }\n"
                 "let n = 0\nlet cur = e\nwhile cur != nil { n += 1\n cur = cur.cause }\nprintln(n)\n", "50\n");
    /* A container as data is shared, not copied. */
    check_output("error E\nlet d = [1]\nlet e = E(\"m\", data: d)\npush(d, 2)\nprintln(len(e.data))\n", "2\n");
}

static void test_cycles(void) {
    const char *msg = "inside itself";
    check_run_error("error E\nlet d = {}\nlet e = E(\"m\", data: d)\nd[\"e\"] = e\n", msg);
    check_run_error("error E\nlet a = []\nlet e = E(\"m\", data: a)\npush(a, e)\n", msg);
    /* Through a nested container and through the cause chain. */
    check_run_error("error E\nlet a = []\nlet e = E(\"m\", data: a)\nlet w = [[e]]\npush(a, w)\n", msg);
    check_run_error("error E\nlet a = []\nlet inner = E(\"i\", data: a)\nlet outer = E(\"o\", cause: inner)\n"
                    "push(a, outer)\n", msg);
    check_run_error("error E\nlet d = {}\nlet inner = E(\"i\", data: d)\nlet outer = E(\"o\", cause: inner)\n"
                    "d[\"x\"] = [outer]\n", msg);
    /* Caught as a ValueError, like the other cycles. */
    check_output("error E\nlet d = {}\nlet e = E(\"m\", data: d)\ntry { d[\"e\"] = e } catch ValueError as v { println(\"refused\") }\nprintln(len(d))\n",
                 "refused\n0\n");
    /* Not a cycle: the same error in two containers, an error in its own data's sibling. */
    check_output("error E\nlet d = {}\nlet e = E(\"m\", data: d)\nlet other = {}\nother[\"e\"] = e\nprintln(len(other))\n", "1\n");
    check_output("error E\nlet e = E(\"m\")\nlet a = [e]\nlet b = [e, a]\nprintln(len(b))\n", "2\n");
    /* Closures in data leave no leak even when they form a cycle through the error. */
    check_output("error E\nfn make() {\n let box = []\n let f = fn() { return len(box) }\n"
                 " push(box, E(\"m\", data: f))\n return 1\n}\nprintln(make())\n", "1\n");
    check_output("error E\nfn make() {\n let holder = nil\n let f = fn() { return holder }\n"
                 " holder = E(\"m\", data: f)\n return 1\n}\nfor i in range(0, 200) { make() }\nprintln(\"ok\")\n", "ok\n");
}

static void test_combinations(void) {
    check_output("error E : ValueError\n"
                 "fn pick(x) { switch x { case 1 { throw E(\"one\") } case 2 { throw IOError(\"two\") } else { return x } } }\n"
                 "for i in [1, 2, 3] { try { println(pick(i)) } catch E as e { println(\"E\", e.message) } catch IOError as e { println(\"IO\", e.message) } }\n",
                 "E one\nIO two\n3\n");
    check_output("error Bad\nfn f(x) { if x > 1 { throw Bad(\"big\", data: x) } return x }\n"
                 "let out = []\nfor i in [1, 2, 3] { try { push(out, f(i)) } catch Bad as b { push(out, b.data * 10) } }\n"
                 "println(out)\n", "[1, 20, 30]\n");
    check_output("error Bad\nlet r = map([1, 2, 3], fn(x) { try { if x == 2 { throw Bad(\"two\") } return x } catch Bad as _ { return -1 } })\nprintln(r)\n",
                 "[1, -1, 3]\n");
    check_output("error Bad\nlet log = []\nfn g() { try { throw Bad(\"x\", data: 1) } finally { push(log, \"fin\") } }\n"
                 "try { g() } catch Bad as b { push(log, b.data) }\nprintln(log)\n", "[\"fin\", 1]\n");
    check_output("error Bad\nfn g() { try { throw Bad(\"inner\") } finally { println(\"f\") } }\n"
                 "try { g() } catch e { println(e.kind) }\n", "f\nBad\n");
    /* Thrown from a callback of map and caught outside. */
    check_output("error Bad\ntry { map([1], fn(x) { throw Bad(\"cb\", data: x) }) } catch Bad as b { println(b.message, b.data) }\n",
                 "cb 1\n");
    /* Kinds in a dict as a lookup of constructors, by name. */
    check_output("error E1\nerror E2\nlet makers = [E1, E2]\nfor m in makers { println(m(\"x\").kind) }\n", "E1\nE2\n");
}

static void test_uncaught_and_messages(void) {
    check_fails("error E\nthrow E(\"top\")\n", 0, 0, LZ_ERR_RUNTIME, "E: top", "");
    check_fails("error E\nthrow E(\"top\", cause: IOError(\"c\"))\n", 0, 0, LZ_ERR_RUNTIME, "E: top", "");
    check_compile_error("error NotFound\nthrow NotFound\n",
                        "an error type must be called to make an error: NotFound(\"message\")", 2, 7);
    check_compile_error("throw IOError\n",
                        "an error type must be called to make an error: IOError(\"message\")", 1, 7);
    check_compile_error("error NotFound\ntry { println(1) } catch NotFond as e { println(2) }\n",
                        "unknown error type 'NotFond'", 2, 26);
    check_compile_error("error A : IOEror\n", "unknown error type 'IOEror'", 1, 11);
    {
        /* The suggestions: the closest kinds, declared ones among them. */
        Program program;
        run(&program, "error NotFound\ntry { } catch NotFond as e { }\n", 0);
        LZ_CHECK(program.diagnostics.count == 1);
        LZ_CHECK(program.diagnostics.items[0].suggestions.count >= 1);
        LZ_CHECK(strcmp(program.diagnostics.items[0].suggestions.names[0], "NotFound") == 0);
        finish(&program);
        run(&program, "error ParseError : ValueError\nerror B : ParseEror\n", 0);
        LZ_CHECK(program.diagnostics.count == 1);
        LZ_CHECK(program.diagnostics.items[0].suggestions.count >= 1);
        LZ_CHECK(strcmp(program.diagnostics.items[0].suggestions.names[0], "ParseError") == 0);
        finish(&program);
        run(&program, "error A : IOEror\n", 0);
        LZ_CHECK(program.diagnostics.items[0].suggestions.count >= 1);
        LZ_CHECK(strcmp(program.diagnostics.items[0].suggestions.names[0], "IOError") == 0);
        finish(&program);
        /* A call of a name that is not there offers the declared kinds as functions. */
        run(&program, "error NotFound\nNotFond(\"x\")\n", 0);
        LZ_CHECK(program.compile_status == LZ_OK);
        LZ_CHECK(program.run_status != LZ_OK);
        LZ_CHECK(strstr(program.vm.error, "NotFond") != NULL);
        const LzNameHint *hint = lz_diagnostics_find_hint(&program.diagnostics, program.vm.error_offset);
        LZ_CHECK(hint != NULL && hint->is_call && hint->suggestions.count >= 1);
        if (hint != NULL && hint->suggestions.count >= 1) {
            LZ_CHECK(strcmp(hint->suggestions.names[0], "NotFound") == 0);
        }
        finish(&program);
    }
    /* A catch of a kind that is declared further down works too. */
    check_output("try { throw Later(\"z\") } catch Later as e { println(e.message) }\nerror Later\n", "z\n");
}

static void test_sandbox_limits(void) {
    /* The limits are not errors a script can catch, whatever kinds it declares. */
    check_fails("error E\nlet n = 0\ntry { while true { n += 1 } } catch Error as _ { println(\"caught\") }\n",
                100000, 0, LZ_ERR_FUEL, "", "");
    check_fails("error E\nlet a = []\ntry { while true { push(a, \"xxxxxxxxxxxxxxxx\") } } catch Error as _ { println(\"caught\") }\n",
                0, 100000, LZ_ERR_HEAP_LIMIT, "heap limit", "");
    check_fails("error E\nfn f() { return f() }\ntry { f() } catch e { println(e.kind) }\n",
                0, 0, LZ_ERR_STACK_OVERFLOW, "stack overflow", "");
}

static void test_many_rounds(void) {
    Program program;
    run(&program,
        "error Bad : ValueError\n"
        "let n = 0\n"
        "for i in range(0, 100000) {\n"
        "    try {\n"
        "        throw Bad(\"x\", cause: IOError(\"c\"), data: {\"i\": i, \"list\": [i, i]})\n"
        "    } catch Bad as e {\n"
        "        n += e.data[\"i\"] - e.data[\"i\"] + 1\n"
        "    }\n"
        "}\n"
        "println(n)\n",
        0);
    LZ_CHECK(program.compile_status == LZ_OK);
    LZ_CHECK(program.run_status == LZ_OK);
    LZ_CHECK(strcmp(program.out.data, "100000\n") == 0);
    finish(&program); /* the stack is balanced and nothing is left alive */
}

int main(void) {
    test_declarations();
    test_hoisting();
    test_declaration_rules();
    test_constructors();
    test_fields_and_chain();
    test_cycles();
    test_combinations();
    test_uncaught_and_messages();
    test_sandbox_limits();
    test_many_rounds();
    return LZ_TEST_RESULT();
}
