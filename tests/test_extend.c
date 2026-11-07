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

static void test_basics(void) {
    check_output("extend string {\n pub fn shout(self) => self.to_upper() + \"!\"\n}\nprintln(\"hi\".shout())\n", "HI!\n");
    check_output("extend int {\n pub fn double(self) => self * 2\n pub get is_even(self) => self % 2 equal 0\n}\n"
                 "let n = 21\nprintln(n.double())\nprintln(n.is_even)\nprintln(4.is_even)\n",
                 "42\nfalse\ntrue\n");
    check_output("class Point {\n pub x: int\n pub y: int\n}\nextend Point {\n"
                 " pub fn dist2(self, other) => (self.x - other.x) * (self.x - other.x) + (self.y - other.y) * (self.y - other.y)\n}\n"
                 "println(Point(0, 0).dist2(Point(3, 4)))\n",
                 "25\n");
    /* every builtin kind */
    check_output("extend float { pub fn half(self) => self / 2.0 }\nextend bool { pub fn flip(self) => not self }\n"
                 "extend array { pub fn second(self) => self[1] }\nextend dict { pub fn size2(self) => len(self) * 2 }\n"
                 "extend nil { pub fn orz(self) => 0 }\nextend function { pub fn twice(self, x) => self(self(x)) }\n"
                 "println(3.0.half(), true.flip(), [1, 2].second(), {\"a\": 1}.size2(), nil.orz())\n"
                 "println((fn(x) => x + 1).twice(5))\n",
                 "1.5 false 2 2 0\n7\n");
    /* lambdas, closures and recursion inside an extension */
    check_output("extend int {\n pub fn fact(self) => if self <= 1 { 1 } else { self * (self - 1).fact() }\n"
                 " pub fn adder(self) { return fn(x) => x + self }\n}\nprintln(5.fact())\nprintln(3.adder()(4))\n",
                 "120\n7\n");
    check_output("extend string { pub fn shout(self) => self.to_upper() + \"!\" }\n"
                 "println([\"a\", \"b\"].map(fn(s) => s.shout()))\n",
                 "[\"A!\", \"B!\"]\n");
    /* bound method */
    check_output("extend string { pub fn late(self) => self + \"?\" }\nlet f = \"abc\".late\nprintln(f())\n", "abc?\n");
    /* the name exists only from the statement on */
    check_run_error("println(\"a\".late())\nextend string { pub fn late(self) => 1 }\n", "undefined variable 'late'");
    check_output("let a = \"x\".len()\nextend string { pub fn late(self) => 1 }\nprintln(a, \"x\".late())\n", "1 1\n");
    /* added after a first call: found at once */
    check_output("fn probe(s) {\n try { return s.later() } catch e { return \"none\" }\n}\n"
                 "println(probe(\"a\"))\nextend string { pub fn later(self) => \"found\" }\nprintln(probe(\"a\"))\n",
                 "none\nfound\n");
}

static void test_privacy(void) {
    check_output("class P { pub x: int }\nextend P {\n fn secret(self) => self.x * 2\n pub fn open(self) => self.secret() + 1\n}\n"
                 "println(P(4).open())\n",
                 "9\n");
    check_run_error("class P { pub x: int }\nextend P {\n fn secret(self) => 1\n}\nP(1).secret()\n", "'secret' is private");
    check_run_error("class P { pub x: int\n n: int = 3 }\nextend P { pub fn peek(self) => self.n }\nP(1).peek()\n",
                    "'n' is private to P");
    /* the help line of an extension reaching a private field */
    {
        Program program;
        run(&program, "class P { pub x: int\n n: int = 3 }\nextend P { pub fn peek(self) => self.n }\nP(1).peek()\n", 0);
        LZ_CHECK(program.run_status != LZ_OK);
        LZ_CHECK(strstr(program.vm.error_help, "an extension can only use the public members of P") != NULL);
        finish(&program);
    }
    /* a private member of one block is not seen by another block */
    check_run_error("extend string {\n fn hid(self) => 1\n pub fn a(self) => self.hid()\n}\n"
                    "extend string {\n pub fn b(self) => self.hid()\n}\nprintln(\"x\".a())\n\"x\".b()\n",
                    "'hid' is private");
}

/* The message and the help line of the first failure of a program. */
static void check_private(const char *text, const char *message, const char *help) {
    Program program;
    run(&program, text, 0);
    bool ok = program.compile_status == LZ_OK && program.run_status != LZ_OK &&
              strstr(program.vm.error, message) != NULL && strstr(program.vm.error_help, help) != NULL;
    if (!ok) {
        fprintf(stderr, "program:\n%s\nexpected [%s] with help [%s]; got status %d [%s] help [%s]\n", text, message,
                help, (int)program.run_status, program.vm.error, program.vm.error_help);
    }
    LZ_CHECK(ok);
    finish(&program);
}

static void test_privacy_in_extend(void) {
    /* an ordinary class: a private field, a private method, a private getter, a private write */
    const char *point = "class Point {\n pub x: int\n y: int = 3\n fn hid(self) => 1\n get tw(self) => self.y * 2\n}\n";
    char text[1024];
    const char *help = "an extension can only use the public members of Point";
    (void)snprintf(text, sizeof text, "%sextend Point { pub fn peek(self) => self.y }\nPoint(1).peek()\n", point);
    check_private(text, "'y' is private to Point", help);
    (void)snprintf(text, sizeof text, "%sextend Point { pub fn peek(self) => self.hid() }\nPoint(1).peek()\n", point);
    check_private(text, "'hid' is private to Point", help);
    (void)snprintf(text, sizeof text, "%sextend Point { pub fn peek(self) => self.tw }\nPoint(1).peek()\n", point);
    check_private(text, "'tw' is private to Point", help);
    (void)snprintf(text, sizeof text, "%sextend Point { pub fn poke(self) { self.y = 9 } }\nPoint(1).poke()\n", point);
    check_private(text, "'y' is private to Point", help);
    (void)snprintf(text, sizeof text, "%sextend Point { pub fn peek(self) => self.with(y: 1) }\nPoint(1).peek()\n", point);
    check_run_error(text, "private");
    /* the public members are fine, and so is the class's own code */
    (void)snprintf(text, sizeof text,
                   "%sextend Point { pub fn dbl(self) => self.x * 2 }\nprintln(Point(4).dbl())\n", point);
    check_output(text, "8\n");

    /* a class of errors */
    const char *bank = "error Broke : RuntimeError {\n pub needed: int\n secret: int = 7\n fn hid(self) => 1\n"
                       " get tw(self) => self.secret * 2\n}\n";
    const char *ehelp = "an extension can only use the public members of Broke";
    (void)snprintf(text, sizeof text, "%sextend Broke { pub fn peek(self) => self.secret }\nBroke(\"m\", 1).peek()\n", bank);
    check_private(text, "'secret' is private to Broke", ehelp);
    (void)snprintf(text, sizeof text, "%sextend Broke { pub fn peek(self) => self.hid() }\nBroke(\"m\", 1).peek()\n", bank);
    check_private(text, "'hid' is private to Broke", ehelp);
    (void)snprintf(text, sizeof text, "%sextend Broke { pub fn peek(self) => self.tw }\nBroke(\"m\", 1).peek()\n", bank);
    check_private(text, "'tw' is private to Broke", ehelp);
    (void)snprintf(text, sizeof text, "%sextend Broke { pub fn poke(self) { self.secret = 1 } }\nBroke(\"m\", 1).poke()\n", bank);
    check_private(text, "'secret' is private to Broke", ehelp);
    (void)snprintf(text, sizeof text, "%sextend Broke { pub fn peek(self) => self.with(secret: 1) }\nBroke(\"m\", 1).peek()\n", bank);
    check_run_error(text, "private");
    (void)snprintf(text, sizeof text,
                   "%sextend Broke { pub fn twice(self) => self.needed * 2 + len(self.message) }\nprintln(Broke(\"m\", 4).twice())\n",
                   bank);
    check_output(text, "9\n");
    (void)snprintf(text, sizeof text, "%sextend Error { pub fn peek(self) => self.copy().secret }\nBroke(\"m\", 1).peek()\n", bank);
    check_private(text, "'secret' is private to Broke", ehelp);
    (void)snprintf(text, sizeof text, "%sextend Error { pub fn peek(self) => self.with(needed: 2).secret }\nBroke(\"m\", 1).peek()\n", bank);
    check_private(text, "'secret' is private to Broke", ehelp);
    /* from an extension of a parent kind, a child's private member is just as closed */
    (void)snprintf(text, sizeof text, "%sextend RuntimeError { pub fn peek(self) => self.secret }\nBroke(\"m\", 1).peek()\n", bank);
    check_private(text, "'secret' is private to Broke", ehelp);
    /* and the other way round: the extension's private members are its own */
    check_output("error E : RuntimeError { pub n: int }\nextend E {\n fn hid(self) => self.n + 1\n pub fn open(self) => self.hid()\n}\n"
                 "println(E(\"m\", 1).open())\n",
                 "2\n");
    check_run_error("error E : RuntimeError { pub n: int }\nextend E {\n fn hid(self) => 1\n}\nE(\"m\", 1).hid()\n",
                    "'hid' is private");

    /* Error and a builtin kind */
    (void)snprintf(text, sizeof text, "%sextend Error { pub fn peek(self) => self.secret }\nBroke(\"m\", 1).peek()\n", bank);
    check_private(text, "'secret' is private to Broke", ehelp);
    (void)snprintf(text, sizeof text, "%sextend Error { pub fn peek(self) => self.hid() }\nBroke(\"m\", 1).peek()\n", bank);
    check_private(text, "'hid' is private to Broke", ehelp);
    (void)snprintf(text, sizeof text,
                   "error V : ValueError {\n pub a: int\n b: int = 2\n}\nextend ValueError { pub fn peek(self) => self.b }\nV(\"m\", 1).peek()\n");
    check_private(text, "'b' is private to V", "an extension can only use the public members of V");
    check_output("error Broke2 : RuntimeError { pub z: int = 1 }\nextend Error { pub fn shout(self) => self.message + \"!\" }\nprintln(ValueError(\"x\").shout(), Broke2(\"y\").shout())\n",
                 "x! y!\n");
    check_output("error E : RuntimeError { pub n: int }\nextend Error { pub fn pub_n(self) => self.n }\nprintln(E(\"m\", 3).pub_n())\n", "3\n");
}

static void test_inheritance(void) {
    check_output("class A { pub fn speak(self) => \"...\" }\nclass D : A { pub override fn speak(self) => \"Woof\" }\n"
                 "extend A { pub fn greet(self) => \"hi from \" + self.speak() }\nprintln(D().greet(), A().greet())\n",
                 "hi from Woof hi from ...\n");
    /* a descendant's own name wins */
    check_output("class A { }\nclass D : A { pub fn z(self) => \"own\" }\nextend A { pub fn z(self) => \"ext\" }\n"
                 "println(D().z(), A().z())\n",
                 "own ext\n");
    /* defined later with the same name: needs override; super reaches the extension */
    check_run_error("class A { }\nextend A { pub fn z(self) => 1 }\nclass D : A { pub fn z(self) => 2 }\n", "hides A.z(); add 'override'");
    check_output("class A { }\nextend A { pub fn z(self) => 1 }\nclass D : A { pub override fn z(self) => super.z() + 10 }\nprintln(D().z())\n",
                 "11\n");
    /* unbound, through the class */
    check_output("class A { pub n: int }\nextend A { pub fn get_n(self) => self.n }\nprintln(A.get_n(A(5)))\n", "5\n");
}

static void test_conflicts(void) {
    check_run_error("extend string { pub fn a(self) => 1 }\nextend string { pub fn a(self) => 2 }\n", "'a' is already defined for string");
    check_run_error("extend string { pub fn to_upper(self) => 1 }\n", "'to_upper' is already defined for string");
    check_run_error("extend array { pub fn first(self) => 1 }\n", "'first' is already defined for array");
    check_run_error("class P { pub fn m(self) => 1 }\nextend P { pub fn m(self) => 2 }\n", "'m' is already defined for P");
    check_run_error("class A { pub fn m(self) => 1 }\nclass B : A { }\nextend B { pub fn m(self) => 2 }\n", "'m' is already defined for B");
    check_run_error("class P { }\nextend P { pub fn to_string(self) => \"x\" }\n", "'to_string' is already defined for P");
    check_run_error("trait T { fn f(self) => 1 }\nclass C : T { }\nextend C { pub fn f(self) => 2 }\n", "'f' is already defined for C");
    /* only a global function of that name: the extension is allowed and preferred */
    check_output("fn double(x) => \"global\"\nextend int { pub fn double(self) => \"ext\" }\nprintln(4.double(), double(4))\n", "ext global\n");
    check_compile_error("extend string { pub fn a(self) => 1\n pub fn a(self) => 2 }\n", "'a' is declared twice in string");
}

static void test_forbidden(void) {
    check_compile_error("fn f() {\n extend string { pub fn a(self) => 1 }\n}\n", "extend can only be used at the top level");
    check_compile_error("if true { extend string { pub fn a(self) => 1 } }\n", "extend can only be used at the top level");
    check_run_error("extend foo { pub fn a(self) => 1 }\n", "'foo' is not a type that can be extended");
    check_run_error("let foo = 3\nextend foo { pub fn a(self) => 1 }\n", "'foo' is not a type that can be extended");
    check_compile_error("extend number { pub fn a(self) => 1 }\n", "'number' is not a type that can be extended");
    check_compile_error("extend int[] { pub fn a(self) => 1 }\n", "extend works on a whole kind such as 'array', not on 'int[]'");
    check_run_error("trait T { fn f(self) }\nextend T { pub fn a(self) => 1 }\n", "extending a trait is not supported yet");
    check_compile_error("extend string { pub x: int }\n", "an extension cannot add fields");
    check_compile_error("extend string { const A = 1 }\n", "an extension cannot add constants");
    check_compile_error("extend string { pub fn make() => 1 }\n", "an extension cannot add static functions yet");
    check_compile_error("extend string { pub fn init(self) => 1 }\n", "an extension cannot add init()");
    check_compile_error("extend string { pub operator +(self, o) => 1 }\n", "operators cannot be added by extend yet");
    check_compile_error("extend string { pub override fn a(self) => 1 }\n", "extend cannot override a method; it can only add new ones");
    check_compile_error("class A { }\nextend A { pub fn a(self) => super.a() }\n", "'super' cannot be used in an extension");
}

static void test_runtime(void) {
    /* the trace names the extension */
    {
        Program program;
        run(&program, "extend string { pub fn boom(self) => 1 / 0 }\n\"x\".boom()\n", 0);
        LZ_CHECK(program.run_status != LZ_OK);
        LZ_CHECK(program.vm.trace_count > 0);
        finish(&program);
    }
    check_output("extend string { pub fn boom(self) => 1 / 0 }\ntry { \"x\".boom() } catch e { println(e.message, e.trace[0]) }\n",
                 "division by zero string.boom (t.lz:1:40)\n");
    /* fuel runs out inside an extension, and cannot be caught */
    check_fails("extend int { pub fn spin(self) { while true { } } }\ntry { 1.spin() } catch e { println(\"caught\") }\n",
                10000, 0, LZ_ERR_FUEL, "instruction limit exceeded", "");
    /* a getter that fails */
    check_output("extend int { pub get bad(self) => 1 / 0 }\ntry { println(3.bad) } catch e { println(e.message) }\n", "division by zero\n");
    /* wrong receiver for an unbound call */
    check_output("class P { pub n: int }\nclass Q { }\nextend P { pub fn g(self) => 1 }\n"
                 "println(P.g(P(1)))\n",
                 "1\n");
}

static void test_memory(void) {
    for (int i = 0; i < 5; i++) {
        Program program;
        run(&program,
            "extend string { pub fn s(self) => self + \"!\" }\nextend int { pub fn d(self) => self * 2 }\n"
            "extend array { pub fn c(self) => len(self) }\nextend dict { pub fn c(self) => len(self) }\n"
            "class K { pub x: int }\nextend K { pub fn m(self) => self.x }\n"
            "let t = 0\nfor i in 0..1000 { t += i.d() + \"a\".s().len() + [i].c() + K(i).m() }\nprintln(t)\n", 0);
        LZ_CHECK(program.run_status == LZ_OK);
        finish(&program);
    }
    /* a block that fails to register leaves nothing behind */
    {
        Program program;
        run(&program, "extend string { pub fn to_upper(self) => 1 }\n", 0);
        LZ_CHECK(program.run_status != LZ_OK);
        finish(&program);
    }
}

static void test_errors(void) {
    check_output("error E : ValueError { pub n: int }\nextend Error { pub fn describe(self) => \"[\" + self.message + \"]\" }\n"
                 "extend ValueError { pub fn vtag(self) => \"V\" }\n"
                 "try { [1][9] } catch e { println(e.describe()) }\n"
                 "try { throw E(\"x\", 1) } catch e { println(e.describe(), e.vtag()) }\n"
                 "try { throw RuntimeError(\"r\").vtag() } catch e { println(e.message) }\n",
                 "[index 9 out of range for array of length 1]\n[x] V\nundefined variable 'vtag'\n");
    check_run_error("extend Error { pub fn message(self) => 1 }\n", "'message' is already defined for Error");
}

int main(void) {
    test_basics();
    test_privacy();
    test_privacy_in_extend();
    test_inheritance();
    test_conflicts();
    test_forbidden();
    test_runtime();
    test_memory();
    test_errors();
    return LZ_TEST_RESULT();
}
