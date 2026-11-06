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

/* The program must not compile; the first error carries this text. */
static void check_compile_error(const char *text, const char *message) {
    Program program;
    run(&program, text, 0);
    bool ok = program.compile_status == LZ_ERR_COMPILE && program.diagnostics.count > 0 &&
              strstr(program.diagnostics.items[0].message, message) != NULL;
    if (!ok) {
        fprintf(stderr, "program:\n%s\nexpected a compile error with [%s]; got status %d [%s]\n", text,
                message, (int)program.compile_status,
                program.diagnostics.count > 0 ? program.diagnostics.items[0].message : "no diagnostic");
    }
    LZ_CHECK(ok);
    /* One mistake is one error: nothing cascades from it. */
    LZ_CHECK(program.diagnostics.count == 1);
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

/* A failed run with a limit: the status, the message, and what was printed before it. */
static void check_fails(const char *text, uint64_t fuel, size_t heap_limit, LzStatus status,
                        const char *message, const char *expected_output) {
    Program program;
    run_limited(&program, text, fuel, heap_limit);
    LZ_CHECK(program.compile_status == LZ_OK);
    bool ok = program.run_status == status && strstr(program.vm.error, message) != NULL;
    const char *actual = program.out.data != NULL ? program.out.data : "";
    if (!ok || strcmp(actual, expected_output) != 0) {
        fprintf(stderr, "program:\n%s\nexpected status %d [%s] and output [%s]; got %d [%s] [%s]\n", text,
                (int)status, message, expected_output, (int)program.run_status, program.vm.error, actual);
        ok = false;
    }
    LZ_CHECK(ok);
    finish(&program);
}

#define BANK \
    "error InsufficientFunds : ValueError {\n pub needed: int\n}\n" \
    "error Overdraft : InsufficientFunds {\n pub account: string = \"main\"\n" \
    " pub get summary(self) => $\"{self.account} short by {self.needed}\"\n}\n" \
    "error Plain : RuntimeError\n" \
    "fn withdraw(balance, amount) {\n if amount > balance {\n" \
    "  throw Overdraft(\"not enough\", needed: amount - balance)\n }\n return balance - amount\n}\n"

static void test_spec_example(void) {
    check_output(BANK
                 "try {\n withdraw(10, 25)\n} catch e {\n println(e.message)\n println(e.needed)\n println(e.summary)\n"
                 " println(e is Overdraft)\n println(e is InsufficientFunds)\n println(e is ValueError)\n"
                 " println(e is TypeError)\n}\n"
                 "try {\n try {\n  withdraw(1, 2)\n } catch inner {\n  throw Plain(\"wrapped\", cause: inner)\n }\n"
                 "} catch outer {\n println(outer.cause.message)\n}\n",
                 "not enough\n15\nmain short by 15\ntrue\ntrue\ntrue\nfalse\nnot enough\n");
    /* positional and named fields are the same */
    check_output(BANK "println(InsufficientFunds(\"x\", 15).needed, InsufficientFunds(\"x\", needed: 15).needed)\n", "15 15\n");
    check_output(BANK "println(Overdraft(\"x\", 1, \"saving\").summary)\n", "saving short by 1\n");
    check_output("error P : RuntimeError\nlet e = P(\"x\", cause: nil, data: 5)\nprintln(e.data, e.message)\n", "5 x\n");
}

static void test_hierarchy(void) {
    check_output("fn probe(f) {\n try { f() } catch e {\n  return [e is TypeError, e is RangeError, e is ArithmeticError, e is RuntimeError, e is Error]\n }\n}\n"
                 "println(probe(fn() => [1][5]))\nprintln(probe(fn() => 1 / (len([]))))\n",
                 "[false, true, false, true, true]\n[false, false, true, true, true]\n");
    check_output("try {\n let a = \"a\"\n let b = 1\n println(a + b)\n} catch e {\n println(e is TypeError)\n}\n", "true\n");
    check_output("error A : ValueError { pub a: int }\nerror B : A\nlet e = B(\"m\", 4)\nprintln(e.a, e is A, e is B, e is ValueError, e is TypeError)\n",
                 "4 true true true false\n");
}

static void test_catch(void) {
    check_output(BANK "try { withdraw(1, 5) } catch ValueError as e { println(\"value\", e.needed) }\n", "value 4\n");
    check_output(BANK "try { withdraw(1, 5) } catch InsufficientFunds as e { println(\"funds\") }\n", "funds\n");
    check_output(BANK "try {\n try { withdraw(1, 5) } catch TypeError as e { println(\"no\") }\n} catch Overdraft as e { println(\"over\") }\n", "over\n");
    check_output(BANK "fn f() {\n try { withdraw(1, 9) } finally { println(\"fin\") }\n}\ntry { f() } catch e { println(e.needed) }\n", "fin\n8\n");
}

static void test_constructor(void) {
    check_run_error("error E : ValueError { pub n: int }\nE()\n", "E(): wrong number of arguments (0 given)");
    check_run_error("error E : ValueError { pub n: int }\nE(\"a\", 1, 2)\n", "E takes at most 1 argument, got 2");
    check_run_error("error E : ValueError { pub n: int }\nE(\"a\", zz: 1)\n", "E has no field 'zz'");
    check_run_error("error E : ValueError { pub n: int }\nE(5)\n", "the message of an error must be a string");
    check_run_error("error E : ValueError { pub n: int }\nE(\"a\")\n", "n");
    check_output("error E : ValueError {\n pub n: int = 3\n fn init(self) {\n  if self.n < 0 { throw ValueError(\"neg\") }\n }\n}\n"
                 "println(E(\"a\").n, E(\"a\", 9).n)\ntry { E(\"a\", -1) } catch e { println(e.message, e is ValueError, e is E) }\n",
                 "3 9\nneg true false\n");
    check_output("error E : Error { pub n: int = 1\n pub fn bump(self) { self.n += 1\n return self.n } }\nlet e = E(\"m\")\nprintln(e.bump(), e.bump(), e.n)\ne.n = 10\nprintln(e.n)\n",
                 "2 3 3\n10\n");
}

static void test_text(void) {
    check_output("error F : RuntimeError { pub a: int\n pub b: string = \"x\" }\nlet f = F(\"hi\", 1)\nprintln(f)\nprintln(f.to_debug())\nprintln(f.to_string())\n",
                 "F: hi\nF(\"hi\", a: 1, b: \"x\")\nF: hi\n");
    check_output("error E : ValueError {\n pub n: int = 3\n pub fn to_string(self) => $\"E<{self.n}>\"\n}\nprintln(E(\"m\"), E(\"m\", 5).to_debug())\n",
                 "E<3> E(\"m\", n: 5)\n");
    check_output("println(ValueError(\"plain\"))\nprintln(ValueError(\"plain\").to_debug())\n", "ValueError: plain\nValueError: plain\n");
}

static void test_declaration_errors(void) {
    check_run_error("class X : Error { }\n", "use 'error X : Error' to declare an error");
    check_run_error("error E\nclass B : E { }\n", "use 'error B : E' to declare an error");
    check_compile_error("class C { }\nerror E : C\n", "'C' is not an error kind");
    check_run_error("error E : ValueError { pub message: string }\n", "E redeclares field 'message' of Error");
    check_run_error("error E : ValueError { pub cause: int }\n", "E redeclares field 'cause' of Error");
    check_output("fn f() => 1\nprintln(f())\nerror Late : RuntimeError\ntry { throw Late(\"x\") } catch Late as e { println(\"ok\") }\n", "1\nok\n");
    /* a bodiless declaration still works before the statement (hoisted) */
    check_output("try { throw Early(\"x\") } catch Early as e { println(e.message) }\nerror Early : RuntimeError\n", "x\n");
}

static void test_typing(void) {
    check_output("error E : Error { pub n: int }\nfn f(e: E) => e.n\nfn g(e: Error) => e.message\nprintln(f(E(\"a\", 3)), g(E(\"a\", 3)), g(ValueError(\"v\")))\n", "3 a v\n");
    check_run_error("error E : Error { pub n: int }\nfn f(e: E) => e.n\nf(ValueError(\"v\"))\n", "expected E");
}

static void test_throw_and_sandbox(void) {
    check_output("error E : RuntimeError { pub n: int }\nfn f() { throw E(\"x\", 1) }\ntry { f() } catch e { println(e.trace[0]) }\n", "f (t.lz:2:10)\n");
    check_output("error E : RuntimeError { pub n: int }\ntry { try { throw E(\"x\", 1) } catch e { throw e } } catch e2 { println(e2.n, e2.message) }\n", "1 x\n");
    check_output("error E : RuntimeError { pub n: int }\nlet e = E(\"x\", 1)\nprintln(len(e.trace))\n", "0\n");
    check_fails("error E : RuntimeError { pub n: int }\ntry { while true { } } catch e { println(\"caught\") } finally { println(\"fin\") }\n",
                10000, 0, LZ_ERR_FUEL, "instruction limit exceeded", "");
    check_output("error E : RuntimeError { pub n: int }\nfor i in 0..3 {\n try { throw E(\"x\", i) } catch e { println(e.n) }\n}\n", "0\n1\n2\n");
    check_output("error E : RuntimeError { pub n: int }\nlet f = fn(k) {\n try { throw E(\"x\", k) } catch e { return e.n * 2 }\n}\nprintln(f(4))\n", "8\n");
    check_run_error("error E : RuntimeError { pub n: int }\nthrow E(\"boom\", 2)\n", "E: boom");
}

static void test_memory(void) {
    for (int i = 0; i < 3; i++) {
        Program program;
        run(&program,
            "error E : RuntimeError { pub n: int\n pub xs: int[] = [] }\n"
            "let c = 0\nfor i in 0..20000 {\n try { throw E(\"x\", i) } catch e { c += e.n }\n}\nprintln(c)\n"
            "let chain = E(\"base\", 0)\nfor i in 0..500 { chain = E(\"n\", i, cause: chain) }\nprintln(chain.n)\n",
            0);
        LZ_CHECK(program.run_status == LZ_OK);
        finish(&program);
    }
    {
        Program program;
        run(&program, "error E : RuntimeError {\n pub n: int\n fn init(self) { throw ValueError(\"in init\") }\n}\ntry { E(\"x\", 1) } catch e { println(e.message) }\n", 0);
        LZ_CHECK(program.run_status == LZ_OK);
        finish(&program);
    }
}

int main(void) {
    test_spec_example();
    test_hierarchy();
    test_catch();
    test_constructor();
    test_text();
    test_declaration_errors();
    test_typing();
    test_throw_and_sandbox();
    test_memory();
    return LZ_TEST_RESULT();
}
