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

static void test_builtin_kinds(void) {
    /* Each built-in failure is of the kind that fits it. */
    check_output("try { let a = [1]\n println(a[5]) } catch e { println(e.kind) }\n", "RangeError\n");
    check_output("try { let d = {\"a\": 1}\n println(d[\"b\"]) } catch e { println(e.kind) }\n", "RangeError\n");
    check_output("try { println(1 / 0) } catch e { println(e.kind, e.message) }\n", "ArithmeticError division by zero\n");
    check_output("fn f(a) { return a + 1 }\ntry { println(f(9223372036854775807)) } catch e { println(e.kind) }\n", "ArithmeticError\n");
    check_output("try { println(int(\"abc\")) } catch e { println(e.kind) }\n", "ValueError\n");
    check_output("fn f(a, b) { return a + b }\ntry { println(f(1, \"a\")) } catch e { println(e.kind) }\n", "TypeError\n");
    check_output("fn f(a) { return a }\nlet g = [f][0]\ntry { g() } catch e { println(e.kind) }\n", "ArityError\n");
    check_output("try { println(nosuch) } catch e { println(e.kind) }\n", "RuntimeError\n");
    check_output("fn f(x) { return x[0] }\ntry { f(5) } catch e { println(e.kind) }\n", "TypeError\n");
    check_output("fn f(x) { return 1 << x }\ntry { f(70) } catch e { println(e.kind) }\n", "RangeError\n");
    /* Error catches every one of them. */
    check_output("fn f(x) { return x[0] }\nlet n = 0\nfor v in [5, [], \"\", nil] { try { f(v) } catch e { n += 1 } }\nprintln(n)\n", "4\n");
    /* The message and the text of an error as printed. */
    check_output("try { println(1 / 0) } catch e { println(e) }\n", "ArithmeticError: division by zero\n");
    check_output("try { println(1 / 0) } catch e { println(e.to_string(), $\"[{e}] [{e.message}]\") }\n",
                 "ArithmeticError: division by zero [ArithmeticError: division by zero] [division by zero]\n");
}

static void test_constructors(void) {
    check_output("println(RuntimeError(\"m\"), RangeError(\"r\").kind, Error(\"e\").message)\n", "RuntimeError: m RangeError e\n");
    check_output("let k = []\nfor e in [Error(\"a\"), RuntimeError(\"a\"), TypeError(\"a\"), ValueError(\"a\"), RangeError(\"a\"), ArithmeticError(\"a\"), ArityError(\"a\"), IOError(\"a\")] { push(k, e.kind) }\nprintln(k)\n",
                 "[\"Error\", \"RuntimeError\", \"TypeError\", \"ValueError\", \"RangeError\", \"ArithmeticError\", \"ArityError\", \"IOError\"]\n");
    check_run_error("println(RuntimeError(5))\n", "the message of an error must be a string");
    check_output("try { RuntimeError(5) } catch e { println(e.kind) }\n", "TypeError\n");
    check_run_error("println(RuntimeError())\n", "wrong number of arguments");
    /* An error is not a dict key, and two errors are equal only if they are the same object. */
    check_output("let a = RuntimeError(\"x\")\nlet b = RuntimeError(\"x\")\nprintln(a == a, a == b)\n", "true false\n");
    check_compile_error("let d = {}\nd[RuntimeError(\"x\")] = 1\n", "an error cannot be a dict key", 2, 2);
    check_output("println(Error(\"a\") in [Error(\"a\")])\n", "false\n");
}

static void test_hierarchy(void) {
    check_output("try { throw RangeError(\"r\") } catch RuntimeError as e { println(\"rt\", e.kind) }\n", "rt RangeError\n");
    check_output("try { throw IOError(\"r\") } catch Error as e { println(\"err\", e.kind) }\n", "err IOError\n");
    check_output("try { throw IOError(\"r\") } catch RuntimeError as e { println(\"no\") } catch IOError as e { println(\"io\") }\n", "io\n");
    check_output("try { throw ValueError(\"v\") } catch RangeError as e { println(\"range\") } catch ValueError as e { println(\"value\") } catch e { println(\"any\") }\n", "value\n");
    check_output("try { throw ArityError(\"v\") } catch RangeError as e { println(\"range\") } catch e { println(\"any\", e.kind) }\n", "any ArityError\n");
    check_output("try { throw Error(\"v\") } catch RuntimeError as _ { println(\"rt\") } catch { println(\"all\") }\n", "all\n");
    check_output("try { throw TypeError(\"v\") } catch TypeError as _ { println(\"type\") }\n", "type\n");
    /* The first catch that matches wins. */
    check_output("try { throw RangeError(\"v\") } catch RuntimeError as e { println(\"first\") } catch Error as e { println(\"second\") }\n", "first\n");
    /* Nothing matches: the error goes on. */
    check_output("try { try { throw IOError(\"io\") } catch RuntimeError as e { println(\"no\") } } catch e { println(\"outer\", e.kind) }\n", "outer IOError\n");
    check_fails("try { throw IOError(\"io\") } catch RuntimeError as e { println(\"no\") }\n", 0, 0, LZ_ERR_RUNTIME, "IOError: io", "");
    /* The variable of a catch: its own, shadowing, gone after. */
    check_output("let e = \"outer\"\ntry { throw Error(\"x\") } catch e { println(e.message) }\nprintln(e)\n", "x\nouter\n");
    check_output("fn f() { let e = 5\n try { throw Error(\"x\") } catch e { println(e.kind) }\n return e }\nprintln(f())\n", "Error\n5\n");
    check_compile_error("try { } catch e { e = 1 }\n", "cannot assign to the catch variable 'e'", 1, 19);
    check_compile_error("try { } catch e { e += 1 }\n", "cannot assign to the catch variable 'e'", 1, 19);
    check_run_error("try { } catch e { }\nprintln(e)\n", "undefined variable 'e'");
    check_output("try { throw Error(\"x\") } catch Error as e { let e2 = e\n println(e2.message) }\n", "x\n");
    check_output("let f = nil\ntry { throw Error(\"kept\") } catch e { f = fn() => e.message }\nprintln(f())\n", "kept\n");
}

static void test_throw(void) {
    check_output("try { throw RuntimeError(\"boom\") } catch e { println(e.message) }\n", "boom\n");
    check_output("let e1 = nil\ntry { try { throw ValueError(\"v\") } catch e { e1 = e\n throw e } } catch e2 { println(e2 == e1, e2.kind) }\n", "true ValueError\n");
    check_output("fn a() { throw ValueError(\"v\") }\nfn b() { a() }\nlet first = nil\ntry { try { b() } catch e { first = e.trace\n throw e } } catch e2 { println(e2.trace == first, len(e2.trace)) }\n", "true 3\n");
    check_compile_error("throw \"oops\"\n", "only an error can be thrown (write RuntimeError(\"oops\"))", 1, 7);
    check_compile_error("throw 5\n", "only an error can be thrown (write RuntimeError(\"oops\"))", 1, 7);
    check_compile_error("throw\n", "expected an error to throw", 1, 6);
    check_run_error("fn f(x) { throw x }\nf(\"s\")\n", "only an error can be thrown");
    check_output("fn f(x) { throw x }\ntry { f(1) } catch e { println(e.kind) }\n", "TypeError\n");
    /* A new error inside a catch, and inside a finally, replaces the old. */
    check_output("try { try { throw Error(\"a\") } catch e { throw ValueError(\"b\") } } catch e { println(e.kind, e.message) }\n", "ValueError b\n");
    check_output("try { try { throw Error(\"a\") } finally { throw ValueError(\"b\") } } catch e { println(e.kind, e.message) }\n", "ValueError b\n");
    check_output("try { try { throw Error(\"a\") } catch e { println(\"c\") } finally { throw RangeError(\"b\") } } catch e { println(e.message) }\n", "c\nb\n");
    /* Thrown from a function called by a function. */
    check_output("fn a() { throw RangeError(\"deep\") }\nfn b() { a()\n println(\"not reached\") }\ntry { b() } catch e { println(e.message) }\n", "deep\n");
    check_fails("throw RangeError(\"oops\")\n", 0, 0, LZ_ERR_RUNTIME, "RangeError: oops", "");
    check_fails("fn f() { throw TypeError(\"t\") }\nf()\n", 0, 0, LZ_ERR_RUNTIME, "TypeError: t", "");
}

static void test_finally(void) {
    check_output("try { println(\"t\") } finally { println(\"f\") }\n", "t\nf\n");
    check_output("try { println(\"t\") } catch e { println(\"c\") } finally { println(\"f\") }\n", "t\nf\n");
    check_output("try { throw Error(\"x\") } catch e { println(\"c\") } finally { println(\"f\") }\n", "c\nf\n");
    check_output("try { try { throw Error(\"x\") } finally { println(\"f\") } } catch e { println(\"outer\") }\n", "f\nouter\n");
    check_output("try { try { throw IOError(\"x\") } catch RuntimeError as e { println(\"no\") } finally { println(\"f\") } } catch e { println(\"outer\") }\n", "f\nouter\n");
    check_fails("try { throw Error(\"x\") } finally { println(\"f\") }\n", 0, 0, LZ_ERR_RUNTIME, "Error: x", "f\n");
    /* return, with its value saved before the finally body runs. */
    check_output("fn f() { try { println(\"t\")\n return \"r\" } finally { println(\"f\") } }\nprintln(f())\n", "t\nf\nr\n");
    check_output("fn f() { let x = 1\n try { return x } finally { x = 2\n println(\"f\", x) } }\nprintln(f())\n", "f 2\n1\n");
    check_output("fn f() { try { return \"try\" } catch e { return \"c\" } finally { println(\"f\") } }\nprintln(f())\n", "f\ntry\n");
    check_output("fn f() { try { throw Error(\"x\") } catch e { return \"c\" } finally { println(\"f\") } }\nprintln(f())\n", "f\nc\n");
    check_output("fn f() { try { try { return 1 } finally { println(\"inner\") } } finally { println(\"outer\") } }\nprintln(f())\n", "inner\nouter\n1\n");
    check_output("fn f() { try { return 1 } finally { try { throw Error(\"in\") } catch e { println(\"caught\", e.message) } } }\nprintln(f())\n", "caught in\n1\n");
    /* break and continue run the finally body of the try inside the loop. */
    check_output("for i in 0..4 { try { if i equal 1 { continue }\n if i equal 3 { break }\n println(\"b\", i) } finally { println(\"f\", i) } }\n",
                 "b 0\nf 0\nf 1\nb 2\nf 2\nf 3\n");
    check_output("let i = 0\nwhile i < 5 { i += 1\n try { if i equal 2 { continue }\n if i equal 4 { break }\n println(\"b\", i) } finally { println(\"f\", i) } }\n",
                 "b 1\nf 1\nf 2\nb 3\nf 3\nf 4\n");
    check_output("for let i = 0; i < 4; i += 1 { try { if i equal 1 { continue }\n if i equal 2 { break } } finally { println(\"f\", i) } }\n", "f 0\nf 1\nf 2\n");
    /* A break leaves only the tries inside its loop. */
    check_output("try { for i in 0..3 { try { break } finally { println(\"inner\") } }\n println(\"after\") } finally { println(\"outer\") }\n", "inner\nafter\nouter\n");
    check_output("for i in 0..2 { try { try { continue } finally { println(\"a\", i) } } finally { println(\"b\", i) } }\n", "a 0\nb 0\na 1\nb 1\n");
    /* The finally body sees the names of where it was written, not of the try. */
    check_output("fn f() { let x = \"outer\"\n try { let x = \"inner\"\n return x } finally { println(x) } }\nprintln(f())\n", "outer\ninner\n");
    check_output("let msg = \"g\"\nfor i in 0..2 { try { let msg = \"inner\"\n continue } finally { println(msg) } }\n", "g\ng\n");
    /* return, break and continue are not allowed in a finally body. */
    check_compile_error("fn f() { try { } finally { return 1 } }\n", "cannot leave a 'finally' block with 'return'", 1, 28);
    check_compile_error("for i in 0..2 { try { } finally { break } }\n", "cannot leave a 'finally' block with 'break'", 1, 35);
    check_compile_error("for i in 0..2 { try { } finally { continue } }\n", "cannot leave a 'finally' block with 'continue'", 1, 35);
    /* ... but a loop inside it may break, and a function inside it may return. */
    check_output("try { } finally { for i in 0..5 { if i equal 2 { break }\n println(i) } }\n", "0\n1\n");
    check_output("try { } finally { fn g() { return 5 }\n println(g()) }\n", "5\n");
    /* An error in the finally body reported once, even with several ways out. */
    {
        Program program;
        run(&program, "fn f() { try { return 1 } finally { println(nope + ) } }\n", 0);
        LZ_CHECK(program.compile_status != LZ_OK);
        LZ_CHECK(program.diagnostics.count == 1);
        finish(&program);
    }
}

static void test_nesting(void) {
    check_output("try { try { throw Error(\"in\") } catch e { println(\"inner\", e.message) } println(\"after\") } catch e { println(\"no\") }\n", "inner in\nafter\n");
    check_output("fn thrower() { throw ValueError(\"deep\") }\nfn mid() { thrower() }\ntry { mid() } catch ValueError as e { println(e.message) }\n", "deep\n");
    check_output("fn count(n) { if n equal 0 { throw RangeError(\"bottom\") }\n try { return count(n - 1) } finally { total += 1 } }\nlet total = 0\ntry { count(1000) } catch e { println(e.message, total) }\n", "bottom 1000\n");
    check_output("fn go(n) { if n equal 0 { return 0 }\n try { return go(n - 1) + 1 } catch e { return -1 } }\nprintln(go(500))\n", "500\n");
    check_output("let n = 0\nlet i = 0\nwhile i < 5 { i += 1\n try { if i % 2 equal 0 { throw Error(\"even\") }\n n += 10 } catch e { n += 1 } }\nprintln(n)\n", "32\n");
    check_output("let n = 0\nfor i in 0..4 { try { throw Error(\"x\") } catch e { n += i } }\nfor let j = 0; j < 3; j += 1 { try { throw Error(\"x\") } catch e { n += 100 } }\nprintln(n)\n", "306\n");
    check_output("fn f(x) { switch x { case 1 { try { throw Error(\"one\") } catch e { return e.message } } else { return \"other\" } } }\nprintln(f(1), f(2))\n", "one other\n");
    check_output("let g = fn(x) { try { return 10 / x } catch e { return e.kind } }\nprintln(g(2), g(0))\n", "5.0 ArithmeticError\n");
    check_output("fn f(x) { return switch x { case 0 => 1 else => 2 } }\ntry { println(f(0)) } finally { println(\"d\") }\n", "1\nd\n");
    /* The stack is cut back to where the try began, whatever was above it. */
    check_output("fn f() { let a = 1\n let b = 2\n try { let c = 3\n let d = [c, 4]\n throw Error(\"x\") } catch e { return a + b } }\nprintln(f())\n", "3\n");
    check_output("fn f(n) { let r = []\n for i in 0..n { try { push(r, 10 / (i - 2)) } catch e { push(r, -1) } }\n return r }\nprintln(f(5))\n", "[-5.0, -10.0, -1, 10.0, 5.0]\n");
    /* Closures over variables of a try body that an error cut short. */
    check_output("let fs = []\nfor i in 0..3 { try { let k = i * 2\n push(fs, fn() => k)\n if i equal 1 { throw Error(\"x\") } } catch e { push(fs, fn() => -1) } }\nlet out = []\nfor f in fs { push(out, f()) }\nprintln(out)\n", "[0, 2, -1, 4]\n");
}

static void test_callbacks(void) {
    check_output("try { println(map([1, 0, 2], fn(x) => 10 / x)) } catch e { println(e.kind) }\n", "ArithmeticError\n");
    check_output("try { println(filter([1, 2], fn(x) => x[0])) } catch e { println(e.kind) }\n", "TypeError\n");
    check_output("try { each([1, 2], fn(x) { if x equal 2 { throw RangeError(\"two\") } }) } catch e { println(e.message) }\n", "two\n");
    check_output("try { println(reduce([1, 2, 3], fn(a, x) => a + 10 / (x - 2), 0)) } catch e { println(e.kind) }\n", "ArithmeticError\n");
    check_output("try { println(find([1, 2], fn(x) => nosuch)) } catch e { println(e.kind) }\n", "RuntimeError\n");
    /* A try inside the callback catches only what happens inside it. */
    check_output("println(map([1, 0, 2], fn(x) { try { return 10 / x } catch e { return -1 } }))\n", "[10.0, -1, 5.0]\n");
    check_output("let log = []\ntry { map([1, 2], fn(x) { try { throw Error(\"inner\") } finally { push(log, x) } }) } catch e { push(log, e.message) }\nprintln(log)\n",
                 "[1, \"inner\"]\n");
    check_output("try { map([1], fn(x) { return map([1, 0], fn(y) => 1 / y) }) } catch e { println(\"nested\", e.kind) }\n", "nested ArithmeticError\n");
    check_output("println(map([1, 2], fn(x) { try { return map([x, 0], fn(y) => 1 / y) } catch e { return \"c\" } }))\n", "[\"c\", \"c\"]\n");
    /* The trace of an error that crossed a callback still names the frames. */
    check_output("fn bad(x) { return 1 / x }\ntry { map([0], bad) } catch e { println(e.trace[0]) }\n", "bad (t.lz:1:22)\n");
    /* try in a function called from a callback, and the error going through two callbacks. */
    check_output("fn safe(x) { try { return 1 / x } catch e { return 0 } }\nprintln(map([0, 1], safe))\n", "[0, 1.0]\n");
    check_output("try { map([1], fn(a) => filter([0], fn(b) => 1 / b)) } catch e { println(e.kind) } finally { println(\"fin\") }\n", "ArithmeticError\nfin\n");
}

/* The trace is captured when the error is made and turned into strings when it is first read. */
static void test_lazy_trace(void) {
    /* never read: nothing may leak, and the heap charge for the captured frames is given back */
    check_output("fn a(n) { throw RangeError(\"m\") }\nfn b(n) { a(n) }\nlet total = 0\n"
                 "for i in 0..50 { try { b(i) } catch e { total += 1 } }\nprintln(total)\n",
                 "50\n");
    /* read twice, the same array both times */
    check_output("fn a() { throw RangeError(\"m\") }\ntry { a() } catch e { println(e.trace == e.trace, len(e.trace), e.trace[0]) }\n",
                 "true 2 a (t.lz:1:10)\n");
    /* kept in an array and read much later, after other errors came and went */
    check_output("fn a(k) { throw RangeError(k) }\nfn b(k) { a(k) }\nlet kept = []\n"
                 "for i in 0..3 { try { b(\"x\") } catch e { kept.push(e) } }\n"
                 "try { b(\"late\") } catch e { println(e.trace[1]) }\n"
                 "for e in kept { println(e.message, e.trace[0], e.trace[1]) }\n",
                 "b (t.lz:2:12)\nx a (t.lz:1:11) b (t.lz:2:12)\nx a (t.lz:1:11) b (t.lz:2:12)\nx a (t.lz:1:11) b (t.lz:2:12)\n");
    /* thrown again without being read: it keeps where it was first made */
    check_output("fn a() { throw ValueError(\"v\") }\nfn b() { a() }\nfn c() { try { b() } catch e { throw e } }\n"
                 "try { c() } catch e2 { println(len(e2.trace), e2.trace[0], e2.trace[2]) }\n",
                 "4 a (t.lz:1:10) c (t.lz:3:17)\n");
    /* more frames than the trace keeps */
    check_output("fn deep(n) { if n == 0 { throw RangeError(\"d\") }\n return deep(n - 1) }\n"
                 "try { deep(40) } catch e { println(len(e.trace)) }\n",
                 "16\n");
}

static void test_fields(void) {
    check_output("try { throw RangeError(\"m\") } catch e { println(e.message, e.kind, len(e.trace)) }\n", "m RangeError 0\n");
    check_output("fn a() { throw RangeError(\"m\") }\nfn b() { a() }\ntry { b() } catch e { println(len(e.trace))\n println(e.trace[0])\n println(e.trace[1])\n println(e.trace[2]) }\n",
                 "3\na (t.lz:1:10)\nb (t.lz:2:11)\n<script> (t.lz:3:8)\n");
    check_output("try { throw Error(\"abc\") } catch e { println(e.message.to_upper(), e.message.len()) }\n", "ABC 3\n");
    check_output("try { throw Error(\"abc\") } catch e { println(e.kind.to_upper(), \"-\" + e.message) }\n", "ERROR -abc\n");
    check_compile_error("try { } catch e { e.message = \"x\" }\n", "cannot assign to a field of an error", 1, 29);
    check_compile_error("try { } catch e { e.message += \"x\" }\n", "cannot assign to a field of an error", 1, 29);
    check_run_error("let x = 5\nprintln(x.foo)\n", "int has no field 'foo'");
    check_run_error("let x = \"s\"\nprintln(x.foo)\n", "string has no field 'foo'");
    check_run_error("try { throw Error(\"x\") } catch e { println(e.nothing) }\n", "error has no field 'nothing'");
    check_output("try { println(5.foo) } catch e { println(e.kind) }\n", "TypeError\n");
    /* Methods with parentheses are as before. */
    check_output("println(\"abc\".to_upper(), [3, 1, 2].len())\n", "ABC 3\n");
    check_output("try { throw Error(\"m\") } catch e { println($\"{e} / {e.message} / {e.kind}\") }\n", "Error: m / m / Error\n");
}

static void test_uncaught(void) {
    check_fails("println(\"a\")\nprintln(1 / 0)\n", 0, 0, LZ_ERR_RUNTIME, "division by zero", "a\n");
    check_fails("fn f() { throw ValueError(\"bad\") }\nf()\n", 0, 0, LZ_ERR_RUNTIME, "ValueError: bad", "");
    check_fails("try { println(1) } finally { println(2) }\nthrow Error(\"late\")\n", 0, 0, LZ_ERR_RUNTIME, "Error: late", "1\n2\n");
    /* The error was never made into a value: nothing is left over. */
    check_fails("fn f() { return 1 / 0 }\nf()\n", 0, 0, LZ_ERR_RUNTIME, "division by zero", "");
}

static void test_sandbox_limits(void) {
    /* The budgets stop the program at once: no catch, no finally. */
    check_fails("try { while true { } } catch e { println(\"caught\") } finally { println(\"fin\") }\n",
                10000, 0, LZ_ERR_FUEL, "instruction limit", "");
    check_fails("try { let a = []\n while true { push(a, \"xxxxxxxxxxxxxxxx\" + string(len(a))) } } catch e { println(\"caught\") } finally { println(\"fin\") }\n",
                0, 200000, LZ_ERR_HEAP_LIMIT, "heap limit", "");
    check_fails("fn r(n) { return r(n + 1) }\ntry { r(0) } catch e { println(\"caught\") } finally { println(\"fin\") }\n",
                0, 0, LZ_ERR_STACK_OVERFLOW, "stack overflow", "");
    check_fails("fn r(n) { return map([1], fn(x) => r(n + 1)) }\ntry { r(0) } catch e { println(\"caught\") } finally { println(\"fin\") }\n",
                0, 0, LZ_ERR_STACK_OVERFLOW, "stack overflow", "");
    check_fails("fn r(n) { try { return r(n + 1) } finally { println(\"f\") } }\nr(0)\n",
                0, 0, LZ_ERR_STACK_OVERFLOW, "stack overflow", "");
    /* An error that can be caught still can, with the same budget around it. */
    {
        Program program;
        run(&program, "try { throw Error(\"x\") } catch e { println(\"c\") } finally { println(\"f\") }\n", 1000);
        LZ_CHECK(program.run_status == LZ_OK);
        LZ_CHECK(strcmp(program.out.data, "c\nf\n") == 0);
        finish(&program);
    }
}

static void test_many_rounds(void) {
    /* The stack and the heap are back where they were after 100000 throws and catches. */
    check_output("let n = 0\nfor i in 0..100000 { try { throw RuntimeError(\"x\") } catch e { n += 1 } }\nprintln(n)\n", "100000\n");
    check_output("let n = 0\nfor i in 0..100000 { try { n += 1 / 0 } catch ArithmeticError as _ { n += 1 } finally { n += 1 } }\nprintln(n)\n", "200000\n");
    check_output("fn f(i) { try { if i % 2 equal 0 { return i }\n throw Error(\"odd\") } catch e { return -i } finally { total += 1 } }\nlet total = 0\nlet s = 0\nfor i in 0..50000 { s += f(i) }\nprintln(s, total)\n",
                 "-25000 50000\n");
}

static void test_compile_errors(void) {
    check_compile_error("try { }\n", "a 'try' needs a 'catch' or a 'finally'", 1, 1);
    check_compile_error("try { } catch e { } catch f { }\n", "a 'catch' after a 'catch' with no error type can never run", 1, 21);
    check_compile_error("try { } catch { } catch RangeError as e { }\n", "a 'catch' after a 'catch' with no error type can never run", 1, 19);
    check_compile_error("try { } catch RangeError { }\n", "'catch RangeError { }' would call the error 'RangeError'; to catch only that kind write 'catch RangeError as _'", 1, 15);
    check_compile_error("try { } catch RangeError e { }\n", "expected 'as' after the error type in 'catch'", 1, 26);
    check_compile_error("try { } catch RangeError as { }\n", "expected a name after 'as'", 1, 29);
    check_compile_error("try { } catch RangeError as 5 { }\n", "expected a name after 'as'", 1, 29);
    check_compile_error("try println(1)\n", "expected '{' after 'try'", 1, 5);
    check_compile_error("try { } catch Rangeerror as e { }\n", "unknown error type 'Rangeerror'", 1, 15);
    check_compile_error("try { } catch NotFound as e { }\n", "unknown error type 'NotFound'", 1, 15);
    check_compile_error("try { } catch RuntimeError as a { } catch RangeError as b { }\n", "unreachable catch: 'RangeError' is already caught by 'RuntimeError' above", 1, 43);
    check_compile_error("try { } catch Error as a { } catch IOError as b { }\n", "unreachable catch: 'IOError' is already caught by 'Error' above", 1, 36);
    check_compile_error("try { } catch IOError as a { } catch IOError as b { }\n", "unreachable catch: 'IOError' is already caught by 'IOError' above", 1, 38);
    /* A reasonable order is fine. */
    check_output("try { throw RangeError(\"r\") } catch RangeError as a { println(\"r\") } catch RuntimeError as b { println(\"rt\") } catch IOError as c { println(\"io\") } catch Error as d { println(\"e\") }\n", "r\n");
    /* A unknown type suggests the closest one. */
    {
        Program program;
        run(&program, "try { } catch Rangeerror as e { }\n", 0);
        LZ_CHECK(program.compile_status != LZ_OK);
        LZ_CHECK(program.diagnostics.count == 1);
        LZ_CHECK(program.diagnostics.items[0].suggestions.count >= 1);
        LZ_CHECK(strcmp(program.diagnostics.items[0].suggestions.names[0], "RangeError") == 0);
        finish(&program);
    }
    /* One mistake, one error. */
    {
        const char *programs[] = {
            "try { } catch RangeError e { }\nprintln(1)\n",
            "try { } catch RangeError { }\nprintln(1)\n",
            "try { } catch e { } catch f { }\nprintln(1)\n",
            "try { }\nprintln(1)\n",
            "throw\nprintln(1)\n",
            "throw 5\nprintln(1)\n",
            "try { } catch Foo as e { }\nprintln(1)\n",
            "try { } catch Error as { }\nprintln(1)\n",
        };
        for (size_t i = 0; i < sizeof programs / sizeof programs[0]; i++) {
            Program program;
            run(&program, programs[i], 0);
            if (program.diagnostics.count != 1) {
                fprintf(stderr, "program:\n%s\nexpected one error, got %d\n", programs[i],
                        (int)program.diagnostics.count);
            }
            LZ_CHECK(program.diagnostics.count == 1);
            finish(&program);
        }
    }
}

static void test_names(void) {
    check_compile_error("let try = 1\n", "expected variable name after 'let'", 1, 5);
    check_compile_error("let catch = 1\n", "expected variable name after 'let'", 1, 5);
    check_compile_error("let finally = 1\n", "expected variable name after 'let'", 1, 5);
    check_compile_error("let throw = 1\n", "expected variable name after 'let'", 1, 5);
    check_output("let trying = 1\nlet catcher = 2\nlet finalize = 3\nlet thrower = 4\nprintln(trying + catcher + finalize + thrower)\n", "10\n");
    /* `as` and `_` mean something only in a catch. */
    check_output("let as = 5\nlet _ = 6\nprintln(as + _)\n", "11\n");
    check_output("fn f(as) { return as }\nprintln(f(1))\n", "1\n");
    check_output("try { throw Error(\"x\") } catch Error as _ { let as = 3\n println(as) }\n", "3\n");
    /* `_` discards: nothing is declared. */
    check_run_error("try { throw Error(\"x\") } catch Error as _ { println(_) }\n", "undefined variable '_'");
}

static void test_in_contexts(void) {
    check_output("fn safe_div(a, b) { try { return a / b } catch ArithmeticError as _ { return nil } }\nprintln(safe_div(6, 3), safe_div(1, 0) ?? \"none\")\n", "2.0 none\n");
    check_output("fn parse(s) { try { return int(s) } catch ValueError as e { return -1 } }\nprintln(map([\"1\", \"x\", \"3\"], parse))\n", "[1, -1, 3]\n");
    check_output("let log = []\nfn work(n) { try { push(log, \"start\")\n if n > 1 { throw ValueError(\"big\") }\n push(log, \"ok\") } catch ValueError as e { push(log, e.message) } finally { push(log, \"done\") } }\nwork(1)\nwork(2)\nprintln(log)\n",
                 "[\"start\", \"ok\", \"done\", \"start\", \"big\", \"done\"]\n");
    check_output("let r = if true { 1 } else { 2 }\ntry { println(r) } catch e { }\n", "1\n");
}

int main(void) {
    test_builtin_kinds();
    test_constructors();
    test_hierarchy();
    test_throw();
    test_finally();
    test_nesting();
    test_callbacks();
    test_fields();
    test_lazy_trace();
    test_uncaught();
    test_sandbox_limits();
    test_many_rounds();
    test_compile_errors();
    test_names();
    test_in_contexts();
    return LZ_TEST_RESULT();
}
