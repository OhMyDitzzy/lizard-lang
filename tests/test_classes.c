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
                 "cannot store: this would create a reference cycle\nValueError\nNode(next: Node(next: nil, items: []), items: [])\n");
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
    check_compile_error("class A : 5 { }", "expected the name of a parent class or trait");
    check_compile_error("class A : int { }", "cannot inherit from a builtin type");
    check_compile_error("class A { override x: int }", "'override' only applies to functions and properties");
    check_compile_error("class A { operator fn f(self) => 1 }",
                        "operator 'fn' cannot be defined");
    check_compile_error("class A { trait x }",
                        "'trait' is not supported yet");
    check_output("pub class A { pub x: int }\nprintln(A(1).x)\n", "1\n");
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
    /* hierarchies: many objects of a class with parents; a construction that fails in the middle of
     * the init chain; a head that fails; the classes and their parents go with the VM */
    check_output("class A { pub a: int = 1 }\nclass B : A { pub b: int = 2 }\nclass C : B { pub c: int = 3 }\n"
                 "let i = 0\nlet total = 0\nwhile i < 100000 { let o = C()\n total += o.a + o.b + o.c\n i += 1 }\n"
                 "println(total)\n",
                 "600000\n");
    check_output("class A { fn init(self) { } }\nclass B : A { pub x: int\n fn init(self) { if self.x > 1 { throw ValueError(\"big\") } } }\n"
                 "let n = 0\nfor i in range(0, 6) { try { B(i) } catch e { n += 1 } }\nprintln(n)\n",
                 "4\n");
    check_run_error("class A { pub fn f(self) => 1 }\nclass B : A { pub fn f(self) => 2 }\n", "hides");
    check_run_error("class A { }\nclass B : A, A { }\n", "only one parent class");
    check_output("class L0 { pub v: int = 0 }\nclass L1 : L0 { }\nclass L2 : L1 { }\nclass L3 : L2 { }\nclass L4 : L3 { }\n"
                 "let x = L4()\nprintln(x.v)\n",
                 "0\n");
}


/* Two sources run one after the other in the SAME vm (A1): the second has no `class`. */
static void check_two_sources_error(const char *first, const char *second, const char *expected,
                                    const char *error) {
    Program program;
    memset(&program, 0, sizeof program);
    lz_vm_init(&program.vm, lz_writer_from_buffer(&program.out), lz_writer_from_buffer(&program.err));
    LZ_CHECK(lz_builtin_install(&program.vm) == LZ_OK);
    LzSource sources[2];
    LzChunk chunks[2];
    const char *texts[2] = {first, second};
    for (int i = 0; i < 2; i++) {
        lz_source_from_string(&sources[i], i == 0 ? "a.lz" : "b.lz", texts[i]);
        program.vm.trace_source = &sources[i];
        lz_chunk_init(&chunks[i]);
        if (i == 1) {
            /* a file that knows of no class: the compiler must not rely on having seen one */
            program.vm.has_classes = false;
        }
        LzDiagnostics diagnostics;
        memset(&diagnostics, 0, sizeof diagnostics);
        LzStatus compiled = lz_compile(&program.vm, &sources[i], &chunks[i], &diagnostics);
        if (i == 1 && error != NULL && compiled != LZ_OK) {
            /* the second source must fail to compile with this message */
            LZ_CHECK(diagnostics.count > 0 && strstr(diagnostics.items[0].message, error) != NULL);
            if (!(diagnostics.count > 0 && strstr(diagnostics.items[0].message, error) != NULL)) {
                fprintf(stderr, "expected compile error [%s]\n", error);
            }
            continue;
        }
        if (compiled != LZ_OK) {
            for (size_t k = 0; k < diagnostics.count; k++) {
                fprintf(stderr, "source %d diagnostic: %s\n", i, diagnostics.items[k].message);
            }
        }
        LZ_CHECK(compiled == LZ_OK);
        if (compiled == LZ_OK) {
            LzStatus ran = lz_vm_run(&program.vm, &chunks[i]);
            if (i == 1 && error != NULL) {
                bool ok = ran != LZ_OK && strstr(program.vm.error, error) != NULL;
                if (!ok) {
                    fprintf(stderr, "expected runtime error [%s], got %d [%s]\n", error, (int)ran, program.vm.error);
                }
                LZ_CHECK(ok);
            } else {
                if (ran != LZ_OK) {
                    fprintf(stderr, "source %d runtime: %s\n", i, program.vm.error);
                }
                LZ_CHECK(ran == LZ_OK);
            }
        }
    }
    const char *actual = program.out.data != NULL ? program.out.data : "";
    if (strcmp(actual, expected) != 0) {
        fprintf(stderr, "expected: [%s]\ngot: [%s]\n", expected, actual);
    }
    LZ_CHECK(strcmp(actual, expected) == 0);
    LZ_CHECK(program.vm.stack_count == 0);
    lz_chunk_free(&program.vm.heap, &chunks[0]);
    lz_chunk_free(&program.vm.heap, &chunks[1]);
    lz_vm_release_roots(&program.vm);
    LZ_CHECK(program.vm.heap.live_objects == 0);
    lz_vm_free(&program.vm);
    lz_buffer_free(&program.out);
    lz_buffer_free(&program.err);
}

static void check_two_sources(const char *first, const char *second, const char *expected) {
    check_two_sources_error(first, second, expected, NULL);
}

static void test_other_file(void) {
    const char *definition =
        "class Point {\n    pub x: int\n    pub y: int = 0\n    pub fn sum(self) => self.x + self.y\n}\n";
    const char *user =
        "let p = Point(x: 3)\n"
        "println(p.x)\n"
        "p.x += 1\n"
        "println(p.sum())\n"
        "let q = p.with(x: 5)\n"
        "println(q.x)\n"
        "println(p == q)\n"
        "println(p == Point(x: 4))\n"
        "println($\"{p}\")\n"
        "fn total(a) => a.sum() + a.y\n"
        "println(total(q))\n";
    check_two_sources(definition, user, "3\n4\n5\nfalse\ntrue\nPoint(x: 4, y: 0)\n5\n");
}

/* A2: constructors, init, getters and to_string nest as deep as ordinary calls. */
static void test_deep_nesting(void) {
    check_output(
        "class R {\n    pub n: int\n"
        "    pub get depth(self) => if self.n == 0 { 0 } else { R(self.n - 1).depth + 1 }\n}\n"
        "println(R(1000).depth)\n",
        "1000\n");
    check_output(
        "class Node {\n    pub next\n    pub v: int\n    pub fn to_string(self) => $\"{self.v}:{self.next}\"\n}\n"
        "class Leaf {\n    pub v: int\n    pub fn to_string(self) => \"end\"\n}\n"
        "let cur = Leaf(0)\nfor i in 1..=1000 { cur = Node(cur, i) }\n"
        "println(len(string(cur)))\n",
        "3896\n");
    check_output(
        "class T {\n    pub depth: int\n    kid = nil\n"
        "    fn init(self) {\n        if self.depth > 0 { self.kid = T(self.depth - 1) }\n    }\n}\n"
        "let t = T(500)\nprintln(\"ok\")\n",
        "ok\n");
    check_fails("class G {\n    pub n: int\n    pub get loop(self) => self.loop\n}\nprintln(G(1).loop)\n", 0, 0,
                LZ_ERR_STACK_OVERFLOW, "stack overflow", "");
    check_fails("class I {\n    pub n: int\n    fn init(self) { let x = I(self.n + 1) }\n}\nI(0)\n", 0, 0,
                LZ_ERR_STACK_OVERFLOW, "stack overflow", "");
    check_fails("class S {\n    pub n: int\n    pub fn to_string(self) => string(self)\n}\nprintln(S(1))\n", 0, 0,
                LZ_ERR_STACK_OVERFLOW, "nested too deeply", "");
}

/* A3: a type names a class by identity, not by name. */
static void test_class_identity(void) {
    const char *first =
        "class A { pub x: int\n pub fn get_x(self) => self.x }\n"
        "fn g(p: A) => p.x\n"
        "let old = A(1)\n";
    /* A is defined again: the old object no longer fits the new A, and says why. */
    check_two_sources_error(first, "class A { pub y: int }\nfn f(p: A) => p\nprintln(f(old))\n", "",
                            "expected A, got A (made by an earlier definition of that class)");
    /* ... and the new object does not fit the old function's A either. */
    check_two_sources_error(first, "class A { pub y: int }\nprintln(g(A(2)))\n", "",
                            "expected A, got A (made by an earlier definition of that class)");
    /* The old object still works with the old class and the old function. */
    check_two_sources(first, "class A { pub y: int }\nfn f2(p: A) => p.y\nprintln(g(old), old.get_x())\n"
                             "println(f2(A(3)))\n",
                      "1 1\n3\n");
    /* A class of an earlier source can be named in a type of a later one. */
    check_two_sources(first, "fn h(p: A) => p.x + 1\nprintln(h(old))\n", "2\n");
    /* A name bound to something that is not a type. */
    check_two_sources_error("let Foo = 5\n", "fn f(p: Foo) => p\n", "", "'Foo' is not a type");
}

/* B1/B2: single inheritance, `override`. */
#define ANIMALS \
    "class Animal {\n    pub name: string\n    pub fn speak(self) => \"...\"\n" \
    "    pub fn intro(self) => $\"I am {self.name} and I say {self.speak()}\"\n}\n" \
    "class Dog : Animal {\n    pub breed: string = \"mixed\"\n    pub override fn speak(self) => \"Woof\"\n}\n"

static char *join2_buffer[4];
static int join2_next = 0;
/* Two source pieces glued into one string that lives until the program ends. */
static const char *join2(const char *a, const char *b) {
    size_t length = strlen(a) + strlen(b) + 1;
    char *text = malloc(length);
    snprintf(text, length, "%s%s", a, b);
    free(join2_buffer[join2_next % 4]); /* the one from four calls ago */
    join2_buffer[join2_next++ % 4] = text;
    return text;
}

static void test_inheritance(void) {
    check_output(ANIMALS "let d = Dog(\"Rex\", breed: \"Husky\")\nprintln(d.intro())\nprintln(d)\n",
                 "I am Rex and I say Woof\nDog(name: \"Rex\", breed: \"Husky\")\n");
    /* positional: the parent's public fields first, then the child's */
    check_output(ANIMALS "println(Dog(\"Rex\", \"Lab\"))\nprintln(Dog(breed: \"Pug\", name: \"Bo\"))\n",
                 "Dog(name: \"Rex\", breed: \"Lab\")\nDog(name: \"Bo\", breed: \"Pug\")\n");
    check_run_error(ANIMALS "Dog()\n", "Dog needs 'name'");
    check_run_error(ANIMALS "Dog(\"a\", \"b\", \"c\")\n", "Dog takes at most 2 arguments, got 3");
    /* a grandchild; the fields of every level */
    check_output(ANIMALS "class Puppy : Dog {\n    pub age: int = 1\n}\nlet p = Puppy(\"Bit\")\n"
                 "println(p, p.intro())\n",
                 "Puppy(name: \"Bit\", breed: \"mixed\", age: 1) I am Bit and I say Woof\n");
    /* redeclared fields and clashing kinds */
    check_run_error("class A { pub name: string }\nclass B : A { name: int }\n", "B redeclares field 'name' of A");
    check_run_error("class A { pub name: string }\nclass B : A { pub fn name(self) => 1 }\n",
                    "'name' is a field of A and cannot be redeclared as a function");
    check_run_error("class A { pub fn f(self) => 1 }\nclass B : A { f: int }\n",
                    "'f' is a function of A and cannot be redeclared as a field");
    /* the head */
    check_run_error("let q = 5\nclass B : q { }\n", "'q' is not a class or a trait");
    check_run_error("class A { }\nclass C { }\nclass B : A, C { }\n",
                    "a class can have only one parent class, and it must come first");
    check_run_error("error E\nclass B : E { }\n", "use 'error B : E' to declare an error");
    check_run_error("class B : Nope { }", "undefined variable 'Nope'");
    /* init runs from the root down; every default is set before the first init */
    check_output("class A {\n    pub a: int = 1\n    fn init(self) { println(\"A\", self.a, self.b) }\n}\n"
                 "class B : A {\n    pub b: int = 2\n    fn init(self) { println(\"B\", self.a, self.b) }\n}\n"
                 "class C : B {\n    pub c: int = 3\n    fn init(self) { println(\"C\", self.a + self.b + self.c) }\n}\n"
                 "C()\n",
                 "A 1 2\nB 1 2\nC 6\n");
    /* a private member of the parent: the child sees it, outsiders do not */
    check_output("class A {\n    secret: int = 7\n    pub fn get_s(self) => self.secret\n}\n"
                 "class B : A {\n    pub fn peek(self) => self.secret\n    pub fn poke(self, other: A) => other.secret\n}\n"
                 "let b = B()\nprintln(b.peek(), b.get_s(), b.poke(A()))\n",
                 "7 7 7\n");
    check_run_error("class A { secret: int = 7 }\nclass B : A { }\nprintln(B().secret)\n", "'secret' is private to A");
    /* the parent does not see the child's private members */
    check_run_error("class A {\n    pub fn look(self) => self.hidden\n}\nclass B : A {\n    hidden: int = 1\n}\n"
                    "println(B().look())\n",
                    "'hidden' is private to B");
    /* pub read: the child may write the parent's field, outsiders may not */
    check_output("class A { pub read n: int = 1 }\nclass B : A {\n    pub fn bump(self) { self.n += 1 }\n}\n"
                 "let b = B()\nb.bump()\nprintln(b.n)\n",
                 "2\n");
    check_run_error("class A { pub read n: int = 1 }\nclass B : A { }\nlet b = B()\nb.n = 5\n",
                    "'n' is read-only outside A");
    /* constants and static functions are inherited */
    check_output("class A {\n    pub const KIND = \"animal\"\n    pub fn make(n: string) => n + \"!\"\n}\nclass B : A { }\n"
                 "println(B.KIND, B.make(\"x\"))\n",
                 "animal x!\n");
    /* Animal.speak(dog) runs the Animal version; Dog.speak(dog) the Dog version */
    check_output(ANIMALS "let d = Dog(\"Rex\")\nprintln(Animal.speak(d), Dog.speak(d))\n", "... Woof\n");
    check_run_error(ANIMALS "Animal.speak(\"x\")\n", "Animal.speak() needs an Animal as its first argument, got string");
    /* with and copy keep the class and run the init chain; == needs the very same class */
    check_output("class A {\n    pub a: int\n    fn init(self) { print(\"A \") }\n}\nclass B : A {\n    pub b: int = 0\n"
                 "    fn init(self) { print(\"B \") }\n}\n"
                 "let x = B(1)\nlet y = x.with(b: 5)\nprintln(y, y == x, x == x.copy(), x == A(1))\n",
                 "A B A B A B(a: 1, b: 5) false true false\n");
    /* type annotations take descendants and refuse siblings */
    check_output(ANIMALS "class Puppy : Dog { }\nfn speak(a: Animal) => a.speak()\nlet x: Animal = Puppy(\"P\")\n"
                 "println(speak(Dog(\"D\")), speak(Puppy(\"P\")), x.speak())\n",
                 "Woof Woof Woof\n");
    check_run_error("class A { }\nclass B : A { }\nclass C : A { }\nfn f(x: B) => x\nf(C())\n", "expected B, got C");
    check_run_error("class A { }\nclass B : A { }\nfn f(x: B) => x\nf(A())\n", "expected B, got A");
}

static void test_override_rules(void) {
    check_run_error("class A { pub fn f(self) => 1 }\nclass B : A { pub fn f(self) => 2 }\n",
                    "B.f() hides A.f(); add 'override'");
    check_run_error("class A { pub fn speak(self) => 1 }\nclass B : A { pub override fn speek(self) => 2 }\n",
                    "B.speek() overrides nothing (did you mean 'speak'?)");
    check_run_error("class A { pub fn f(self) => 1 }\nclass B : A { pub override fn f(self, y) => 2 }\n",
                    "B.f() must take the same parameters as A.f() (expected 0, got 1)");
    check_run_error("class A { pub fn f(self) => 1 }\nclass B : A { override fn f(self) => 2 }\n",
                    "B.f() cannot be private because A.f() is public");
    check_output("class A { fn f(self) => 1\n pub fn g(self) => self.f() }\nclass B : A {\n    pub override fn f(self) => 2\n}\n"
                 "println(B().g())\n",
                 "2\n"); /* a private parent method can be overridden, and may become public */
    check_run_error("class A { pub fn f(self) => 1 }\nclass B : A { pub override get f(self) => 2 }\n",
                    "B.f is a property but A.f is a function");
    check_run_error("class A { pub get f(self) => 1 }\nclass B : A { pub override fn f(self) => 2 }\n",
                    "B.f is a function but A.f is a property");
    check_run_error("class A { pub fn f() => 1 }\nclass B : A { pub override fn f(self) => 2 }\n",
                    "B.f is a function but A.f is a static function");
    check_output("class A { pub get area(self) => 1 }\nclass B : A { pub override get area(self) => 2 }\n"
                 "println(A().area, B().area)\n",
                 "1 2\n");
    check_output("class A { pub fn make() => 1 }\nclass B : A { pub override fn make() => 2 }\nprintln(A.make(), B.make())\n",
                 "1 2\n");
    check_compile_error("class A { pub override x: int }", "'override' only applies to functions and properties");
    check_compile_error("class A { pub const K = 1\n override const L = 2 }", "'override' only applies to functions and properties");
}

static void test_super(void) {
    const char *chain =
        "class Animal {\n    pub name: string\n    pub fn speak(self) => \"...\"\n    pub get label(self) => \"animal\"\n}\n"
        "class Dog : Animal {\n    pub override fn speak(self) => super.speak() + \"Woof\"\n"
        "    pub override get label(self) => super.label + \"/dog\"\n}\n"
        "class Puppy : Dog {\n    pub override fn speak(self) => super.speak() + \"!\"\n"
        "    pub fn later(self) => (fn() => super.speak())()\n}\n";
    /* Dog.speak's super is Animal even when self is a Puppy */
    check_output(join2(chain, "let p = Puppy(\"Bit\")\nprintln(p.speak())\nprintln(p.label)\nprintln(Dog(\"d\").speak())\n"
                              "println(p.later())\n"),
                 "...Woof!\nanimal/dog\n...Woof\n...Woof\n");
    /* a private method of the parent is reachable */
    check_output("class A { fn hidden(self) => 5 }\nclass B : A { pub fn g(self) => super.hidden() }\nprintln(B().g())\n", "5\n");
    check_compile_error("println(super.x())", "'super' can only be used inside a method");
    check_compile_error("fn f() => super.x()", "'super' can only be used inside a method");
    check_compile_error("class A { pub fn f(self) => super.f() }", "A has no parent class, so 'super' has nothing to refer to");
    check_compile_error("class A { }\nclass B : A { pub fn g() => super.g() }", "'super' cannot be used in a static function");
    check_compile_error("class A { }\nclass B : A { pub fn g(self) => super(1) }", "'super' must be followed by .name");
    check_compile_error("class A { }\nclass B : A { pub fn g(self) => super.init() }",
                        "init() runs automatically; super.init() is not needed");
    check_compile_error("class A { }\nclass B : A { pub fn g(self) { let s = super\n return s } }", "'super' must be followed by .name");
    check_compile_error("class A { pub x: int = 1 }\nclass B : A { pub y: int = super.m() }", "'super' can only be used inside a method");
    check_run_error("class A { }\nclass B : A { pub fn h(self) => super.x() }\nB().h()\n", "the parent of B has no method 'x'");
    check_output("class A { pub get p(self) => 1\n pub fn f(self) => 2 }\nclass B : A {\n pub fn g(self) => super.p()\n"
                 " pub fn h(self) => super.f }\ntry { B().g() } catch e { println(e.message) }\n"
                 "try { B().h() } catch e { println(e.message) }\n",
                 "'p' is a property; write super.p without parentheses\n'f' is a function; write super.f() with parentheses\n");
}

static void test_is(void) {
    const char *zoo = "class Animal { }\nclass Dog : Animal { }\nclass Puppy : Dog { }\nclass Cat : Animal { }\nlet d = Puppy()\n";
    check_output(join2(zoo, "println(d is Animal, d is Dog, d is Puppy, d is Cat)\nprintln(d is not Cat, d is not Puppy)\n"),
                 "true true true false\ntrue false\n");
    check_output(join2(zoo, "println(5 is int, \"a\" is string, 5 is string, 2.5 is float, nil is Dog, nil is nil)\n"),
                 "true true false true false true\n");
    check_output(join2(zoo, "println([1, 2] is int[], [1, \"a\"] is int[], {\"a\": 1} is dict, 5 is Dog, \"x\" is Animal)\n"),
                 "true false true false false\n");
    check_output(join2(zoo, "if d is Animal and 1 > 0 { println(\"both\") }\nprintln(d is Cat or d is Dog)\n"
                            "fn kind(x) => if x is Dog { \"dog\" } else if x is Cat { \"cat\" } else { \"other\" }\n"
                            "println(kind(d), kind(Cat()), kind(3))\n"),
                 "both\ntrue\ndog cat other\n");
    /* the test is the annotation's test */
    check_output(join2(zoo, "fn accepts(x) { try { let y: Dog = x\n return true } catch e { return false } }\n"
                            "println(accepts(d) == (d is Dog), accepts(Cat()) == (Cat() is Dog), accepts(nil) == (nil is Dog))\n"),
                 "true true true\n");
    /* `is` is a word only after an expression: a program that used it as a name still runs */
    check_output("let is = 3\nprintln(is + 1)\nfn f(is) => is * 2\nprintln(f(4))\nlet x = 5\nprintln(x is int, is)\n", "4\n8\ntrue 3\n");
    check_compile_error("let d = 1\nprintln(d is int == true)", "'is' cannot be chained with '==', '!=' or another 'is'; add parentheses");
    check_compile_error("let d = 1\nprintln(d == true is bool)", "'is' cannot follow '==' or '!='; add parentheses");
    check_compile_error("let d = 1\nprintln(d is foo)", "'foo' is not a type");
    check_compile_error("let d = 1\nprintln(d is int is int)", "'is' cannot be chained");
    check_output("println((1 is int) == true, (1 is string) != true)\n", "true true\n");
}

#define SHAPE_TRAIT \
    "trait Shape {\n    fn area(self)\n    get label(self)\n    fn describe(self) => $\"{self.label}: area = {self.area()}\"\n}\n"

static void test_traits(void) {
    check_output(join2(SHAPE_TRAIT, "class Square : Shape {\n    pub side: int\n    pub fn area(self) => self.side * self.side\n"
                                    "    pub get label(self) => \"square\"\n}\nlet s = Square(3)\nprintln(s.describe())\n"
                                    "println(s is Shape, 5 is Shape, nil is Shape)\nprintln(Shape)\n"
                                    "fn show(x: Shape) => x.describe()\nprintln(show(s))\n"),
                 "square: area = 9\ntrue false false\n<trait Shape>\nsquare: area = 9\n");
    /* a default runs on the real receiver: describe uses the area of the class */
    check_output(join2(SHAPE_TRAIT, "class Sq : Shape {\n pub s: int\n pub fn area(self) => self.s * self.s\n pub get label(self) => \"sq\"\n}\n"
                                    "class Ci : Shape {\n pub r: int\n pub fn area(self) => 3 * self.r * self.r\n pub get label(self) => \"ci\"\n}\n"
                                    "println(Sq(2).describe(), Ci(1).describe())\n"),
                 "sq: area = 4 ci: area = 3\n");
    /* the contract */
    check_run_error(join2(SHAPE_TRAIT, "class C : Shape { pub get label(self) => 1 }\n"), "C does not implement 'area' required by Shape");
    check_run_error(join2(SHAPE_TRAIT, "class C : Shape { pub fn area(self, x) => 1\n pub get label(self) => 1 }\n"),
                    "C.area() must take the same parameters as Shape.area() (expected 0, got 1)");
    check_run_error(join2(SHAPE_TRAIT, "class C : Shape { fn area(self) => 1\n pub get label(self) => 1 }\n"),
                    "C.area() must be public because Shape requires it");
    check_run_error(join2(SHAPE_TRAIT, "class C : Shape { pub fn area(self) => 1\n pub label: string = \"x\" }\n"),
                    "'label' is required as a 'get' property; a field cannot satisfy it");
    check_run_error(join2(SHAPE_TRAIT, "class C : Shape { pub fn area(self) => 1\n pub fn label(self) => 1 }\n"),
                    "C.label is a function but Shape.label is a property");
    /* what a trait cannot be or hold */
    check_run_error(join2(SHAPE_TRAIT, "Shape()\n"), "Shape is a trait and cannot be created; use a class that implements it");
    check_compile_error("trait T { x: int }", "a trait cannot have fields");
    check_compile_error("trait T { const K = 1 }", "a trait cannot have constants yet");
    check_compile_error("trait T { fn f() => 1 }", "a trait cannot have static functions");
    check_compile_error("trait T { fn init(self) => 1 }", "a trait cannot have init()");
    check_compile_error("trait T { fn f(self) => super.g() }", "'super' cannot be used in a trait");
    check_compile_error("class C { fn f(self) }", "this function needs a body (only a trait can leave it out)");
    check_output("trait T { pub fn f(self) => 1 }\nclass C : T { }\nprintln(C().f())\n", "1\n"); /* `pub` in a trait is accepted */
    /* a default must not reach the private members of the class */
    check_run_error("trait A { fn f(self) => self.secret }\nclass C : A { secret: int = 3 }\nprintln(C().f())\n", "'secret' is private to C");
    /* the class wins over the default; override is required for a default, optional for a requirement */
    check_run_error("trait A { fn f(self) => 1 }\nclass C : A { pub fn f(self) => 3 }\n", "C.f() hides A.f(); add 'override'");
    check_output("trait A { fn f(self) => 1 }\nclass C : A { pub override fn f(self) => 3 }\nprintln(C().f())\n", "3\n");
    check_output("trait A { fn f(self) }\nclass C : A { pub override fn f(self) => 3 }\nclass D : A { pub fn f(self) => 4 }\n"
                 "println(C().f(), D().f())\n",
                 "3 4\n");
    check_run_error("trait A { fn f(self) }\nclass C : A { pub override fn g(self) => 3 }\n", "C.g() overrides nothing");
    /* two traits with one name: the class must choose; the same trait by two paths is no conflict */
    check_run_error("trait A { fn f(self) => 1 }\ntrait B { fn f(self) => 2 }\nclass C : A, B { }\n",
                    "C gets 'f' from both A and B; define f() in C to choose");
    check_output("trait A { fn f(self) => 1 }\ntrait B { fn f(self) => 2 }\nclass C : A, B { pub override fn f(self) => 3 }\nprintln(C().f())\n",
                 "3\n");
    check_run_error("trait A { fn f(self) => 1 }\ntrait B { fn f(self) }\nclass C : A, B { }\n", "C gets 'f' from both A and B");
    check_output("trait D { fn f(self) => 1 }\ntrait A : D { }\ntrait B : D { }\nclass C : A, B { }\nprintln(C().f())\n", "1\n");
    check_output("trait A { fn f(self) => 1 }\nclass P : A { }\nclass C : P, A { }\nprintln(C().f(), C() is A)\n", "1 true\n");
    /* the trait of a parent class counts; so does a parent that satisfies a contract */
    check_output("trait A { fn f(self) => 1 }\nclass P : A { }\nclass C : P { }\nprintln(C().f(), C() is A)\n", "1 true\n");
    check_output("trait N { fn name(self) }\nclass P { pub fn name(self) => \"p\" }\nclass C : P, N { }\nprintln(C().name(), C() is N)\n", "p true\n");
    /* a trait that extends another */
    check_output("trait Named { fn name(self) }\ntrait Describable : Named { fn describe(self) => $\"I am {self.name()}\" }\n"
                 "class Tag : Describable { pub fn name(self) => \"tag\" }\n"
                 "println(Tag().describe(), Tag() is Named, Tag() is Describable)\n",
                 "I am tag true true\n");
    check_run_error("trait Named { fn name(self) }\ntrait Describable : Named { fn describe(self) => 1 }\nclass Tag : Describable { }\n",
                    "Tag does not implement 'name' required by Named");
    /* head errors */
    check_run_error("trait A { fn f(self) => 1 }\nclass C : A, A { }\n", "trait 'A' is listed twice in the head of C");
    check_run_error("class K { }\ntrait T : K { }\n", "a trait can only extend other traits");
    check_run_error("class K { }\ntrait T { fn f(self) => 1 }\nclass C : T, K { }\n",
                    "a class can have only one parent class, and it must come first");
    /* a class with a parent and a trait: the parent first */
    check_output("trait T { fn hello(self) => \"hi \" + self.name }\nclass A { pub name: string }\nclass B : A, T { }\nprintln(B(\"x\").hello())\n",
                 "hi x\n");
    /* with, copy, ==, printing are not touched by traits */
    check_output(join2(SHAPE_TRAIT, "class Sq : Shape {\n pub s: int\n pub fn area(self) => self.s\n pub get label(self) => \"q\"\n}\n"
                                    "let a = Sq(2)\nprintln(a, a.with(s: 3), a == a.copy(), a == Sq(2))\n"),
                 "Sq(s: 2) Sq(s: 3) true true\n");
}

#define VEC_CLASS \
    "class Vec {\n    pub x: int\n    pub y: int\n\n    pub operator +(self, other) => Vec(self.x + other.x, self.y + other.y)\n" \
    "    pub operator -(self) => Vec(-self.x, -self.y)\n    pub operator *(self, k) => Vec(self.x * k, self.y * k)\n" \
    "    pub operator <(self, other) => self.x * self.x + self.y * self.y < other.x * other.x + other.y * other.y\n}\n"

static void test_operators(void) {
    check_output(join2(VEC_CLASS, "println(Vec(1, 2) + Vec(3, 4) * 2)\nprintln(-Vec(1, 2))\n"
                                  "println(Vec(1, 1) < Vec(2, 2), Vec(2, 2) >= Vec(1, 1), Vec(1, 1) <= Vec(1, 1), Vec(3, 3) > Vec(1, 1))\n"),
                 "Vec(x: 7, y: 10)\nVec(x: -1, y: -2)\ntrue true true true\n");
    /* compound assignment through a variable, a field and an element */
    check_output(join2(VEC_CLASS, "let v = Vec(1, 1)\nv += Vec(1, 1)\nv *= 3\nprintln(v)\n"
                                  "class Holder { pub v: Vec\n}\nlet h = Holder(Vec(1, 0))\nh.v += Vec(0, 5)\nprintln(h.v)\n"
                                  "let a = [Vec(1, 1)]\na[0] += Vec(5, 5)\nprintln(a)\n"),
                 "Vec(x: 6, y: 6)\nVec(x: 1, y: 5)\n[Vec(x: 6, y: 6)]\n");
    /* operator == replaces the content comparison, wherever equality is used */
    check_output("class Vec {\n    pub x: int\n    pub operator ==(self, other) => other is Vec and self.x == other.x\n}\n"
                 "println(Vec(1) == Vec(1), Vec(1) != Vec(1), Vec(1) == 5, 5 == Vec(1))\n"
                 "println(Vec(2) in [Vec(1), Vec(2)], Vec(9) not in [Vec(1)], [Vec(1)] == [Vec(1)], {\"a\": Vec(1)} == {\"a\": Vec(1)})\n"
                 "switch Vec(3) { case Vec(3) => println(\"switch\")\n else => println(\"none\") }\n",
                 "true false false false\ntrue true true true\nswitch\n");
    check_output("class Pt {\n    pub x: int\n    pub operator ==(self, other) => true\n}\nclass Box { pub p: Pt\n}\nprintln(Box(Pt(1)) == Box(Pt(2)))\n", "true\n");
    check_run_error("class A {\n    pub x: int\n    pub operator ==(self, o) => 1\n}\nA(1) == A(1)\n", "operator == of A must return a bool, got int");
    check_run_error("class A {\n    pub x: int\n    pub operator <(self, o) => 1\n}\nA(1) < A(1)\n", "operator < of A must return a bool, got int");
    /* what is not defined */
    check_run_error(join2(VEC_CLASS, "println(Vec(1, 1) / 2)\n"), "operator '/' is not defined for Vec");
    check_run_error(join2(VEC_CLASS, "println(2 * Vec(1, 1))\n"), "Vec defines '*' only when it is on the left; write vec * 2");
    check_run_error("class A { pub x: int }\nA(1) < A(2)\n", "operator '<' is not defined for A");
    check_run_error("class A { pub x: int }\n-A(1)\n", "operator '-' is not defined for A");
    check_run_error("class A { pub x: int }\nA(1) + 1\n", "operator '+' is not defined for A");
    check_run_error("class A {\n pub x: int\n pub operator <(self, o) => true\n}\nA(1) > 5\n", "operator '<' is not defined for int");
    /* forbidden definitions, wrong parameter counts */
    check_compile_error("class A { pub operator >(self, o) => 1 }", "operator '>' cannot be defined; define '<' and '>', '<=' and '>=' follow from it");
    check_compile_error("class A { pub operator <=(self, o) => 1 }", "operator '<=' cannot be defined");
    check_compile_error("class A { pub operator !=(self, o) => 1 }", "'!=' follows from '=='");
    check_compile_error("class A { pub operator ?\?(self, o) => 1 }", "operator '?\?' cannot be defined");
    check_compile_error("class A { pub operator and(self, o) => 1 }", "operator 'and' cannot be defined");
    check_compile_error("class A { pub operator +(self) => 1 }", "operator + takes 'self' and one other value");
    check_compile_error("class A { pub operator +(self, a, b) => 1 }", "operator + takes 'self' and one other value");
    check_compile_error("class A { pub operator +(a, b) => 1 }", "an operator takes 'self' first");
    check_compile_error("trait T { operator +(self, o) => 1 }", "operators in traits are not supported yet");
    check_output("class A { pub operator: int = 5 }\nprintln(A().operator)\n", "5\n"); /* `operator` is a word only here */
    /* inheritance: operators are inherited; redefining one needs override; privacy */
    check_output("class V { pub x: int\n pub operator +(self, o) => V(self.x + o.x) }\nclass W : V { }\nprintln(W(1) + W(2))\n", "V(x: 3)\n");
    check_run_error("class V { pub x: int\n pub operator +(self, o) => 1 }\nclass W : V { pub operator +(self, o) => 2 }\n", "hides");
    check_output("class V { pub x: int\n pub operator +(self, o) => 1 }\nclass W : V { pub override operator +(self, o) => 2 }\nprintln(W(1) + W(2), V(1) + V(2))\n", "2 1\n");
    check_run_error("class P { pub x: int\n operator +(self, o) => 1 }\nP(1) + P(2)\n", "'+' is private to P");
    check_output("class P { pub x: int\n operator +(self, o) => 7\n pub fn add(self, o) => self + o }\nprintln(P(1).add(P(2)))\n", "7\n");
    /* an error inside the operator surfaces with its trace; endless recursion ends in the depth error */
    check_fails("class B { pub x: int\n pub operator +(self, o) => 1 / 0 }\nfn f(a, b) => a + b\nf(B(1), B(2))\n", 0, 0,
                LZ_ERR_RUNTIME, "division by zero", "");
    check_fails("class R { pub x: int\n pub operator +(self, o) => self + o }\nR(1) + R(2)\n", 0, 0, LZ_ERR_STACK_OVERFLOW,
                "stack overflow", "");
    check_fails("class R { pub x: int\n pub operator +(self, o) => o + o }\nlet r = R(1)\nwhile true { r + r }\n", 200, 0, LZ_ERR_FUEL,
                "instruction limit exceeded", "");
    /* a function that does not use classes still adds objects (the slow path of + knows them) */
    check_output("fn sum(a, b) => a + b\nclass W { pub n: int\n pub operator +(self, o) => W(self.n + o.n) }\n"
                 "println(sum(W(1), W(2)), sum(1, 2), sum(\"a\", \"b\"))\n",
                 "W(n: 3) 3 ab\n");
    check_output("fn less(a, b) => a < b\nclass W { pub n: int\n pub operator <(self, o) => self.n < o.n }\nprintln(less(W(1), W(2)), less(2, 1))\n",
                 "true false\n");
    /* sorting and extremes of objects use operator < */
    check_output("class V { pub x: int\n pub operator <(self, o) => self.x < o.x }\nlet a = [V(3), V(1), V(2)]\nsort(a)\nprintln(a)\n"
                 "println(min(a), max(a))\n",
                 "[V(x: 1), V(x: 2), V(x: 3)]\nV(x: 1) V(x: 3)\n");
    check_output("class Bad { pub x: int\n pub operator <(self, o) => 1 / 0 }\nlet b = [Bad(1), Bad(2)]\n"
                 "try { sort(b) } catch e { println(e.message) }\nprintln(b)\n",
                 "division by zero\n[Bad(x: 1), Bad(x: 2)]\n");
    check_run_error("class A { pub x: int }\nsort([A(2), A(1)])\n", "operator '<' is not defined for A");
}

int main(void) {
    test_operators();
    test_traits();
    test_is();
    test_super();
    test_inheritance();
    test_override_rules();
    test_class_identity();
    test_other_file();
    test_deep_nesting();
    test_programs();
    test_limits();
    test_memory();
    return LZ_TEST_RESULT();
}
