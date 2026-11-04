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

static void test_programs(void) {
    check_output("class Point {\n    pub x: int\n    pub y: int = 0\n    pub fn sum(self) => self.x + self.y\n    pub fn move(self, dx, dy) {\n        self.x += dx\n        self.y += dy\n    }\n    pub fn origin() => Point(0, 0)\n    pub get len2(self) => self.x * self.x + self.y * self.y\n}\nlet p = Point(3, 4)\nprintln(p.sum())\np.move(1, 1)\nprintln(p)\nprintln(Point(x: 3))\nprintln(p == Point(4, 5), identical(p, Point(4, 5)))\nprintln(p.with(x: 10))\n",
                 "7\nPoint(x: 4, y: 5)\nPoint(x: 3, y: 0)\ntrue false\nPoint(x: 10, y: 5)\n");
    check_output("class Point {\n    pub x: int\n    pub y: int = 0\n    pub fn sum(self) => self.x + self.y\n    pub fn move(self, dx, dy) {\n        self.x += dx\n        self.y += dy\n    }\n    pub fn origin() => Point(0, 0)\n    pub get len2(self) => self.x * self.x + self.y * self.y\n}\nlet f = Point(1, 2).sum\nprintln(f(), Point.origin(), Point(3, 4).len2)\nprintln([Point(1, 2), Point(3, 4)].map(Point.sum), Point)\nprintln(f)\n",
                 "3 Point(x: 0, y: 0) 25\n[3, 7] <class Point>\n<method Point.sum>\n");
    check_output("class Account {\n    pub read owner: string\n    pub read balance: int = 0\n    count: int = 0\n    pub fn deposit(self, amount: int) {\n        if amount <= 0 { throw ValueError(\"amount must be positive\") }\n        self.balance += amount\n        self.count += 1\n    }\n    pub get deposits(self) => self.count\n    pub fn to_string(self) => $\"{self.owner}: {self.balance}\"\n}\nlet a = Account(\"Sari\")\na.deposit(100)\na.deposit(50)\nprintln(a)\nprintln($\"{a} after {a.deposits} deposits\")\nprintln([a])\ntry { a.balance = 5 } catch e { println(e.message) }\ntry { a.deposit(-1) } catch e { println(e.message) }\n",
                 "Sari: 150\nSari: 150 after 2 deposits\n[Account(owner: \"Sari\", balance: 150, count: 2)]\n'balance' is read-only outside Account\namount must be positive\n");
    check_output("class Bag { pub items: int[] = [] }\nlet a = Bag()\nlet b = Bag()\na.items.push(1)\nprintln(a.items, b.items)\n",
                 "[1] []\n");
    check_output("class Point {\n    pub x: int\n    pub y: int = 0\n    pub fn sum(self) => self.x + self.y\n    pub fn move(self, dx, dy) {\n        self.x += dx\n        self.y += dy\n    }\n    pub fn origin() => Point(0, 0)\n    pub get len2(self) => self.x * self.x + self.y * self.y\n}\nlet p = Point(1, 2)\nlet q = p.copy()\nq.x = 9\nprintln(p, q, p.copy() == p, identical(p.copy(), p))\nprintln(Point(1) in [Point(1), Point(2)], Point(1) != Point(2), Point(1) not equal Point(1))\nprintln(Point(1) == 1, Point(1, 2) == [1, 2])\nswitch p { case Point(1, 2) { println(\"hit\") } else { println(\"miss\") } }\n",
                 "Point(x: 1, y: 2) Point(x: 9, y: 2) true false\ntrue true false\nfalse false\nhit\n");
    check_output("class T { pub celsius: float\n    kelvin: float = 0.0\n    fn init(self) {\n        if self.celsius < -273.15 { throw ValueError(\"below absolute zero\") }\n        self.kelvin = self.celsius + 273.15\n    }\n    pub get k(self) => self.kelvin }\nprintln(T(0.0).k)\ntry { T(-300.0) } catch e { println(e.message) }\nlet t = T(1.0)\nprintln(t.with(celsius: 2.0).k, t.copy().k)\ntry { t.init() } catch e { println(e.message) }\n",
                 "273.15\nbelow absolute zero\n275.15 274.15\ninit() runs automatically when an object is created\n");
    check_output("class N { pub a: int\n  b: int }\ntry { N(1) } catch e { println($\"{e.kind}: {e.message}\") }\n",
                 "RuntimeError: field 'b' of N has no value yet (assign it in init())\n");
    check_output("class Point {\n    pub x: int\n    pub y: int = 0\n    pub fn sum(self) => self.x + self.y\n    pub fn move(self, dx, dy) {\n        self.x += dx\n        self.y += dy\n    }\n    pub fn origin() => Point(0, 0)\n    pub get len2(self) => self.x * self.x + self.y * self.y\n}\ntry { Point() } catch e { println($\"{e.kind}: {e.message}\") }\ntry { Point(1, 2, 3) } catch e { println($\"{e.kind}: {e.message}\") }\ntry { Point(1, zzzz: 2) } catch e { println($\"{e.kind}: {e.message}\") }\ntry { Point(1, x: 2) } catch e { println($\"{e.kind}: {e.message}\") }\ntry { Point(\"a\") } catch e { println($\"{e.kind}: {e.message}\") }\ntry { Point(1).x = \"s\" } catch e { println($\"{e.kind}: {e.message}\") }\n",
                 "ArityError: Point needs 'x'\nArityError: Point takes at most 2 arguments, got 3\nArityError: Point has no field 'zzzz'\nArityError: 'x' was given twice\nTypeError: Point.x expects int, got string\nTypeError: Point.x expects int, got string\n");
    check_output("class Point {\n    pub x: int\n    pub y: int = 0\n    pub fn sum(self) => self.x + self.y\n    pub fn move(self, dx, dy) {\n        self.x += dx\n        self.y += dy\n    }\n    pub fn origin() => Point(0, 0)\n    pub get len2(self) => self.x * self.x + self.y * self.y\n}\nlet p = Point(1)\ntry { p.zz = 1 } catch e { println($\"{e.kind}: {e.message}\") }\ntry { p.len2 = 1 } catch e { println($\"{e.kind}: {e.message}\") }\ntry { p.len2() } catch e { println($\"{e.kind}: {e.message}\") }\ntry { p.origin() } catch e { println($\"{e.kind}: {e.message}\") }\ntry { Point.sum(\"s\") } catch e { println($\"{e.kind}: {e.message}\") }\ntry { let d = {p: 1} } catch e { println($\"{e.kind}: {e.message}\") }\n",
                 "RuntimeError: Point has no field 'zz'\nRuntimeError: 'len2' is a computed property and cannot be assigned\nRuntimeError: 'len2' is a property; write r.len2 without parentheses\nRuntimeError: 'origin' is a static function; call it as Point.origin()\nTypeError: Point.sum() needs a Point as its first argument, got string\nTypeError: a Point cannot be a dict key\n");
    check_output("class A {\n    pub read balance: int = 0\n    pub read items: int[] = [1]\n    count: int = 0\n    fn helper(self) => 1\n    pub fn peek(self, o: A) => o.count + o.helper()\n    pub fn later(self) { let f = fn() => self.count\n      return f() }\n    pub const MAX = 10\n    const SECRET = 5\n}\nlet a = A()\nprintln(a.peek(A()), a.later(), A.MAX)\ntry { println(a.count) } catch e { println(e.message) }\ntry { a.helper() } catch e { println(e.message) }\ntry { println(A.SECRET) } catch e { println(e.message) }\ntry { a.balance += 1 } catch e { println(e.message) }\ntry { a.items[0] = 1 } catch e { println(e.message) }\ntry { A.MAX = 3 } catch e { println(e.message) }\n",
                 "1 0 10\n'count' is private to A\n'helper' is private to A\n'SECRET' is private to A\n'balance' is read-only outside A\n'items' is read-only outside A\n'MAX' is a constant and cannot be assigned\n");
    check_output("class B { pub n: int = 0\n  secret: int = 1 }\nclass C { pub n: int = 0\n  pub fn sneak(self, b: B) => b.secret }\ntry { println(C().sneak(B())) } catch e { println(e.message) }\n",
                 "'secret' is private to B\n");
    check_output("class Color {\n    const RED = Color(255, 0, 0)\n    pub const WHITE = Color(255, 255, 255)\n    pub r: int\n    pub g: int\n    pub b: int\n    pub fn gray(level) => Color(level, level, level)\n}\nprintln(Color.WHITE.r, Color.gray(5))\n",
                 "255 Color(r: 5, g: 5, b: 5)\n");
    check_output("class P { pub x: int\n  pub fn to_string(self) => $\"P<{self.x}>\" }\nlet p = P(1)\nprintln(string(p), p.to_string(), $\"{p}\", $\"{p:>6}|\", $\"{p=}\", [p])\n",
                 "P<1> P<1> P<1>   P<1>| p=P(x: 1) [P(x: 1)]\n");
    check_output("class Plain { pub name: string\n  pub inner: int = 2 }\nprintln(Plain(\"Sari\"))\nprintln({\"k\": Plain(\"a\")}, Plain(\"b\").to_debug())\n",
                 "Plain(name: \"Sari\", inner: 2)\n{\"k\": Plain(name: \"a\", inner: 2)} Plain(name: \"b\", inner: 2)\n");
    check_output("class Bad { pub x: int = 1\n  pub fn to_string(self) => 42 }\ntry { println(Bad()) } catch e { println($\"{e.kind}: {e.message}\") }\nclass Thrower { pub x: int = 1\n  pub fn to_string(self) { throw ValueError(\"nope\") } }\ntry { println($\"{Thrower()}\") } catch e { println($\"{e.kind}: {e.message}\") }\n",
                 "TypeError: to_string() of Bad must return a string, got int\nValueError: nope\n");
    check_output("class Node { pub next: any = nil\n  pub items: any[] = [] }\nlet a = Node()\nlet b = Node(next: a)\ntry { a.next = b } catch e { println(e.message) }\ntry { a.items.push(a) } catch e { println(e.kind) }\nprintln(b)\n",
                 "cannot store: this would create a reference cycle (through field 'next' of Node)\nValueError\nNode(next: Node(next: nil, items: []), items: [])\n");
    check_output("class H { pub handler: any = nil\n  pub n: int = 7\n  pub fn wire(self) { self.handler = fn() => self.n } }\nlet h = H()\nh.wire()\nprintln(h.handler())\n",
                 "7\n");
    check_output("class C { pub n: int = 0\n  pub fn inc(self) { self.n += 1 }\n  pub fn tick(self) => fn() { self.inc() } }\nlet c = C()\nlet t = c.tick()\nt()\nt()\nprintln(c.n)\n",
                 "2\n");
    check_output("class In { pub v: int = 1 }\nclass Out { pub inner: In = In()\n  pub items: int[] = [1, 2, 3] }\nlet o = Out()\no.inner.v = 5\no.inner.v += 1\no.items[1] += 10\no.items[0] = 9\nprintln(o)\nlet calls = 0\nfn make() { calls += 1\n  return o }\nmake().inner.v += 1\nprintln(calls, o.inner.v)\n",
                 "Out(inner: In(v: 6), items: [9, 12, 3])\n1 7\n");
    check_output("let read = 1\nlet get = 2\nlet pub = 3\nlet class = 4\nprintln(read + get + pub + class)\n",
                 "10\n");
    check_output("class A { pub read: int = 5\n  pub get: int = 6\n  pub read r: int = 7 }\nlet a = A(1, 2, 3)\nprintln(a)\ntry { a.r = 1 } catch e { println(e.message) }\na.read = 9\nprintln(a.read)\n",
                 "A(read: 1, get: 2, r: 3)\n'r' is read-only outside A\n9\n");
    check_output("class Q { pub n: int = 0 }\nlet xs = [Q(1), Q(2)]\nlet total = 0\nfor q in xs { total += q.n }\nprintln(total, Q(1) ?? 3, Q(0) == Q(0))\n",
                 "3 Q(n: 1) true\n");
    check_compile_error("fn f() { class A { } }",
                        "classes can only be declared at the top level");
    check_compile_error("class A { pub x: int\n pub x: int }",
                        "'x' is declared twice in A");
    check_compile_error("class A { println(1) }",
                        "only fields, functions, properties and constants can appear in a class body");
    check_compile_error("class A { fn f(a, self) => 1 }",
                        "'self' must be the first parameter of a method");
    check_compile_error("class A { fn f() => self }",
                        "static functions have no self");
    check_compile_error("class A { x: int = self.y }",
                        "a field default cannot use 'self'");
    check_compile_error("class A : B { }",
                        "inheritance is not supported yet");
    check_compile_error("class A { override fn f(self) => 1 }",
                        "'override' is not supported yet");
    check_compile_error("class A { operator fn f(self) => 1 }",
                        "'operator' is not supported yet");
    check_compile_error("class A { trait x }",
                        "'trait' is not supported yet");
    check_compile_error("pub class A { }",
                        "'pub' on a class needs modules");
    check_compile_error("class A { pub get a(self, b) => 1 }",
                        "a 'get' property takes only 'self'");
    check_compile_error("class A { fn init(self, b) { } }",
                        "init() takes only 'self'");
    check_run_error("class A { }\nlet a = A()\nprintln(a.zzz)",
                    "A has no field");
}

static void test_limits(void) {
    /* The fuel runs out inside init(), the heap fills with objects: sandbox errors, not crashes. */
    check_fails("class B { fn init(self) { let i = 0\n while i < 1000000 { i += 1 } } }\nprintln(\"go\")\nB()\nprintln(\"no\")\n",
                5000, 0, LZ_ERR_FUEL, "instruction limit", "go\n");
    check_fails("class B { pub v: int = 0 }\nlet xs = []\nlet i = 0\nwhile i < 1000000 { xs.push(B(i))\n i += 1 }\n",
                0, 200 * 1024, LZ_ERR_HEAP_LIMIT, "heap limit", "");
    /* A to_string that calls itself hits the depth limit of callbacks. */
    check_fails("class L { pub x: int = 1\n pub fn to_string(self) => $\"{self}\" }\nprintln(L())\n", 0, 0,
                LZ_ERR_STACK_OVERFLOW, "nested too deeply", "");
}

static void test_memory(void) {
    /* Many short-lived objects, failed constructions and closures over self leave nothing behind
     * (finish() checks that no object is alive). */
    check_output("class Box { pub v: any = 0 }\nlet i = 0\nwhile i < 20000 { let b = Box(Box(i))\n i += 1 }\nprintln(i)\n",
                 "20000\n");
    check_output("class Bad { pub x: int\n fn init(self) { if self.x > 1 { throw ValueError(\"big\") } } }\n"
                 "let n = 0\nfor i in range(0, 5) { try { Bad(i) } catch e { n += 1 } }\nprintln(n)\n",
                 "3\n");
}

int main(void) {
    test_programs();
    test_limits();
    test_memory();
    return LZ_TEST_RESULT();
}
