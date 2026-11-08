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
    check_run_error("error E : ValueError { pub n: int }\nE(\"a\", 1, 2)\n", "E takes at most 2 arguments (message, n), got 3");
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

#define DESCRIBE "trait Describe {\n fn describe(self) => $\"<{self.message}>\"\n}\n"

static void test_traits(void) {
    check_output(DESCRIBE \
                 "error E : RuntimeError, Describe { pub n: int }\n"
                 "let e = E(\"m\", 3)\nprintln(e.describe(), e is Describe, e is E, e is RuntimeError, e.n)\n"
                 "try { throw e } catch x { println(x is Describe, x.describe()) }\n"
                 "println(ValueError(\"v\") is Describe)\n",
                 "<m> true true true 3\ntrue <m>\nfalse\n");
    check_output("trait Need {\n fn need(self)\n}\n"
                 "error E : RuntimeError, Need {\n pub fn need(self) => 7\n}\nprintln(E(\"m\").need())\n", "7\n");
    check_output("trait A {\n fn a(self) => 1\n}\ntrait B {\n fn b(self) => 2\n}\n"
                 "error E : ValueError, A, B\nlet e = E(\"m\")\nprintln(e.a(), e.b(), e is A, e is B, e is ValueError)\n",
                 "1 2 true true true\n");
    check_output(DESCRIBE "error P : RuntimeError, Describe\nerror C : P\nprintln(C(\"z\").describe(), C(\"z\") is Describe)\n",
                 "<z> true\n");
    check_run_error("trait T {\n fn x(self)\n}\nerror E : RuntimeError, T\n", "E does not implement 'x' required by T");
    check_run_error("trait A {\n fn x(self) => 1\n}\ntrait B {\n fn x(self) => 2\n}\nerror E : RuntimeError, A, B\n",
                    "E gets 'x' from both A and B");
    check_run_error("class K { }\nerror E : RuntimeError, K\n", "a class can have only one parent class");
}

static void test_arity_message(void) {
    check_run_error("error InsufficientFunds : ValueError { pub needed: int }\nInsufficientFunds(\"m\", 1, 2)\n",
                    "InsufficientFunds takes at most 2 arguments (message, needed), got 3");
    /* cause and data are named, never positional, so they are not counted */
    check_run_error("error E : ValueError { pub n: int }\nE(\"m\", 1, 2, cause: nil, data: 3)\n",
                    "E takes at most 2 arguments (message, n), got 3");
    check_output("error E : ValueError { pub n: int }\nprintln(E(\"m\", 1, cause: nil, data: 3).data)\n", "3\n");
    check_run_error("error E : ValueError { }\nE(\"m\", 1)\n", "E takes at most 1 argument (message), got 2");
}

static void test_copy_and_with(void) {
    /* with fields: everything is copied, trace is not, init does not run */
    check_output("error F : RuntimeError {\n pub a: int\n pub b: string = \"x\"\n}\n"
                 "fn boom() { throw F(\"hi\", 1, \"y\", cause: ValueError(\"c\"), data: [1, 2]) }\n"
                 "try { boom() } catch e {\n let c = e.copy()\n println(c.message, c.a, c.b, c.cause.message, c.data, c is F, c is RuntimeError)\n"
                 " println(len(e.trace) > 0, len(c.trace))\n println(c == e, c.kind)\n}\n",
                 "hi 1 y c [1, 2] true true\ntrue 0\nfalse F\n");
    /* the copy is independent of the original */
    check_output("error F : RuntimeError { pub a: int }\nlet e = F(\"m\", 1)\nlet c = e.copy()\nc.a = 9\nprintln(e.a, c.a)\n", "1 9\n");
    /* copy does not run init */
    check_output("let n = [0]\nerror F : RuntimeError {\n pub a: int\n fn init(self) { n[0] += 1 }\n}\n"
                 "let e = F(\"m\", 1)\nlet c = e.copy()\nprintln(n[0])\nlet d = e.with(a: 2)\nprintln(n[0], d.a, e.a)\n", "1\n2 2 1\n");
    /* with: fields, message, cause, data */
    check_output("error F : RuntimeError { pub a: int\n pub b: int = 0 }\nlet e = F(\"m\", 1, 2)\n"
                 "let d = e.with(a: 5, message: \"n\", data: 7, cause: ValueError(\"c\"))\n"
                 "println(d.message, d.a, d.b, d.data, d.cause.message, e.message, e.a, e.data, e.cause)\n",
                 "n 5 2 7 c m 1 nil nil\n");
    /* plain and builtin errors */
    check_output("error P : RuntimeError\nlet p = P(\"p\", data: 1)\nprintln(p.copy().message, p.copy().data, p.copy() is P)\n"
                 "println(p.with(message: \"q\").message, p.with(message: \"q\") is P, p.message)\n"
                 "println(ValueError(\"x\").with(message: \"y\"), ValueError(\"x\").copy(), ValueError(\"x\").with(data: 3).data)\n",
                 "p 1 true\nq true p\nValueError: y ValueError: x 3\n");
    /* a kind without a body that inherits a class */
    check_output("error F : RuntimeError { pub a: int }\nerror G : F\nlet g = G(\"m\", 4)\nprintln(g.copy().a, g.with(a: 6).a, g.with(a: 6) is G)\n",
                 "4 6 true\n");
    /* with runs init, and a failure in init is thrown */
    check_output("error F : RuntimeError {\n pub a: int\n fn init(self) {\n  if self.a < 0 { throw ValueError(\"neg\") }\n }\n}\n"
                 "let e = F(\"m\", 1)\ntry { e.with(a: -1) } catch x { println(x.message, x is ValueError, x is F) }\nprintln(e.a)\n",
                 "neg true false\n1\n");
    /* with checks types and names */
    check_run_error("error F : RuntimeError { pub a: int }\nF(\"m\", 1).with(a: \"s\")\n", "a");
    check_run_error("error F : RuntimeError { pub a: int }\nF(\"m\", 1).with(zz: 1)\n", "F has no field 'zz'");
    check_run_error("error F : RuntimeError { pub a: int }\nF(\"m\", 1).with(message: 5)\n", "the message of an error must be a string");
    check_run_error("error F : RuntimeError { pub a: int }\nF(\"m\", 1).with(cause: 5)\n", "the cause of an error must be an error or nil");
    check_run_error("error F : RuntimeError { pub n: int\n priv: int = 0 }\nF(\"m\", 1).with(priv: 1)\n", "private");
    /* positional arguments */
    check_run_error("error F : RuntimeError { pub a: int }\nF(\"m\", 1).with(2)\n", "with() takes named fields, like e.with(needed: 5)");
    check_run_error("error F : RuntimeError { pub a: int }\nF(\"m\", 1).with(2, a: 3)\n", "with() takes named fields, like e.with(needed: 5)");
    check_run_error("ValueError(\"x\").with(\"y\")\n", "with() takes named fields, like e.with(needed: 5)");
    check_output("try { ValueError(\"x\").with(\"y\") } catch e { println(e.kind, e is ArityError) }\n", "ArityError true\n");
    /* a thrown copy gets its own trace */
    check_output("error F : RuntimeError { pub a: int }\nfn f(e) { throw e.copy() }\ntry { f(F(\"m\", 1)) } catch x { println(x.trace[0], x.a) }\n",
                 "f (t.lz:2:11) 1\n");
}

/* `error K0 : RuntimeError`, then each kind under the one `parent(i)` names (always a smaller i). */
static char *kind_program(int count, int (*parent)(int), const char *tail) {
    size_t cap = (size_t)count * 40 + strlen(tail) + 64;
    char *text = malloc(cap);
    LZ_CHECK(text != NULL);
    size_t used = 0;
    for (int i = 0; i < count; i++) {
        int up = parent(i);
        if (up < 0) {
            used += (size_t)snprintf(text + used, cap - used, "error K%d : RuntimeError\n", i);
        } else {
            used += (size_t)snprintf(text + used, cap - used, "error K%d : K%d\n", i, up);
        }
    }
    (void)snprintf(text + used, cap - used, "%s", tail);
    return text;
}

static int chain_parent(int i) {
    return i - 1;
}

static int branch_parent(int i) {
    return i == 0 ? -1 : (i - 1) / 2;
}

static void test_kind_limit(void) {
    /* 200 kinds in a chain: every ancestor matches, no descendant does */
    {
        char *text = kind_program(200, chain_parent,
                                  "let e = K150(\"m\")\n"
                                  "let up = 0\nlet down = 0\n"
                                  "println(e is K0, e is K149, e is K150, e is K151, e is K199, e is RuntimeError, e is TypeError)\n"
                                  "let last = K199(\"z\")\n"
                                  "println(last is K0, last is K100, last is K198, last is K199, last is Error)\n"
                                  "try { throw last } catch K120 as x { println(\"caught\", x.kind) }\n");
        check_output(text, "true true true false false true false\ntrue true true true true\ncaught K199\n");
        free(text);
    }
    /* 200 kinds in a binary tree: ancestors match, siblings and cousins do not */
    {
        char *text = kind_program(200, branch_parent,
                                  "let e = K100(\"m\")\n"
                                  /* 100 -> 49 -> 24 -> 11 -> 5 -> 2 -> 0 */
                                  "println(e is K100, e is K49, e is K24, e is K11, e is K5, e is K2, e is K0)\n"
                                  /* siblings and the other branch */
                                  "println(e is K101, e is K99, e is K50, e is K1, e is K3, e is K6, e is K12)\n"
                                  /* a child of K100 is K201 (does not exist); K100's own children are 201 and 202 */
                                  "let leaf = K199(\"m\")\n"
                                  /* 199 -> 99 -> 49 -> 24 -> 11 -> 5 -> 2 -> 0 */
                                  "println(leaf is K99, leaf is K49, leaf is K2, leaf is K0, leaf is K100, leaf is K198)\n");
        check_output(text,
                     "true true true true true true true\n"
                     "false false false false false false false\n"
                     "true true true true false false\n");
        free(text);
    }
    /* up to the limit works: 8 built-in kinds and 248 declared ones */
    {
        char *text = kind_program(248, chain_parent, "println(K247(\"m\") is K0, K247(\"m\") is RuntimeError)\n");
        check_output(text, "true true\n");
        free(text);
    }
    /* past it, a message, not a crash or a wrong match */
    {
        char *text = kind_program(249, branch_parent, "");
        check_compile_error(text, "too many error kinds (limit is 256)");
        free(text);
        text = kind_program(400, chain_parent, "println(1)\n");
        Program program;
        run(&program, text, 0);
        LZ_CHECK(program.compile_status == LZ_ERR_COMPILE);
        LZ_CHECK(program.diagnostics.count > 0);
        LZ_CHECK(strstr(program.diagnostics.items[0].message, "too many error kinds (limit is 256)") != NULL);
        finish(&program);
        free(text);
    }
}

static void test_cycles(void) {
    const char *cycle = "cannot store: this would create a reference cycle";
    const char *x = "error X : RuntimeError {\n pub items: any = []\n pub fn grow(self) { self.items = [self] }\n"
                    " pub fn grow_nested(self) { self.items = [[self]] }\n"
                    " pub fn grow_cause(self) { self.items = X(\"n\", cause: self) }\n"
                    " pub fn grow_data(self) { self.items = X(\"n\", data: [self]) }\n"
                    " pub fn fine(self) { self.items = [1, [2]] }\n}\n";
    char text[1024];
    /* the error is `self`: an array holding it, stored in its own field, is a cycle */
    (void)snprintf(text, sizeof text, "%slet e = X(\"m\")\ne.grow()\n", x);
    check_run_error(text, cycle);
    (void)snprintf(text, sizeof text, "%slet e = X(\"m\")\ne.grow_nested()\n", x);
    check_run_error(text, cycle);
    /* an error made with the first one as cause, stored back into it */
    (void)snprintf(text, sizeof text, "%slet e = X(\"m\")\ne.grow_cause()\n", x);
    check_run_error(text, cycle);
    (void)snprintf(text, sizeof text, "%slet e = X(\"m\")\ne.grow_data()\n", x);
    check_run_error(text, cycle);
    /* from outside, the same */
    (void)snprintf(text, sizeof text, "%slet e = X(\"m\")\ne.items = [e]\n", x);
    check_run_error(text, cycle);
    (void)snprintf(text, sizeof text, "%slet e = X(\"m\")\ne.items = e\n", x);
    check_run_error(text, cycle);
    (void)snprintf(text, sizeof text, "%slet e = X(\"m\")\ne.items = {\"k\": [e]}\n", x);
    check_run_error(text, cycle);
    /* the data and the cause of an error are given when it is made: they cannot be assigned */
    check_run_error("error X : RuntimeError { pub n: int = 0 }\nlet e = X(\"m\")\ne.data = [e]\n", "cannot assign to field 'data' of error");
    check_run_error("error X : RuntimeError { pub n: int = 0 }\nlet e = X(\"m\")\ne.data = e\n", "cannot assign to field 'data' of error");
    /* a cycle caught and the error kept: nothing leaks (finish checks live_objects) */
    (void)snprintf(text, sizeof text, "%slet e = X(\"m\")\ntry { e.grow() } catch k { println(k.message) }\nprintln(e.items)\n", x);
    check_output(text, "cannot store: this would create a reference cycle\n[]\n");
    /* the valid versions work */
    (void)snprintf(text, sizeof text, "%slet e = X(\"m\")\ne.fine()\nprintln(e.items)\n", x);
    check_output(text, "[1, [2]]\n");
    (void)snprintf(text, sizeof text,
                   "%slet a = X(\"a\")\nlet b = X(\"b\", cause: a, data: [a])\nb.items = [a, a.copy()]\n"
                   "a.items = [1]\nprintln(b.cause.message, len(b.items), b.data[0].message)\n", x);
    check_output(text, "a 2 a\n");
    /* a chain of causes stored in a field of the first error's copy */
    (void)snprintf(text, sizeof text,
                   "%slet a = X(\"a\")\nlet b = X(\"b\", cause: a)\nlet c = a.copy()\nc.items = b\nprintln(c.items.cause.message)\n", x);
    check_output(text, "a\n");
    /* an error inside an instance inside the error's own field */
    check_run_error("class Box { pub v: any }\nerror X : RuntimeError { pub items: any = []\n"
                    " pub fn grow(self) { self.items = Box(self) } }\nlet e = X(\"m\")\ne.grow()\n",
                    "reference cycle");
    /* many errors made and dropped, with the heap checked at the end */
    for (int i = 0; i < 2; i++) {
        Program program;
        (void)snprintf(text, sizeof text,
                       "%sfor i in 0..2000 {\n let e = X(\"m\")\n try { e.grow() } catch k { }\n e.fine()\n}\nprintln(1)\n", x);
        run(&program, text, 0);
        LZ_CHECK(program.run_status == LZ_OK);
        finish(&program);
    }
}

int main(void) {
    test_cycles();
    test_kind_limit();
    test_arity_message();
    test_copy_and_with();
    test_traits();
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
