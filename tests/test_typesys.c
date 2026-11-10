#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "lizard/builtin.h"
#include "lizard/compiler.h"
#include "lizard/debug.h"
#include "lizard/interface.h"
#include "test_util.h"

/*
 * The gradual type system on top of plain Lizard: function types, generic functions, narrowing
 * and inferred results. Programs are run whole; a compile error is matched by a piece of its
 * message, and `check_help` also looks for a piece of its help line.
 */

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

/* Fuel and heap limit of 0 mean unlimited. */
static void run(Program *program, const char *text, uint64_t fuel, size_t heap_limit) {
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


/* Compiles, prints `expected`, then stops with a type error whose text holds `message`. */
static void check_type_error(const char *text, const char *expected, const char *message) {
    Program program;
    run(&program, text, 0, 0);
    LZ_CHECK(program.compile_status == LZ_OK);
    LZ_CHECK(program.run_status == LZ_ERR_TYPE);
    if (strstr(program.vm.error, message) == NULL) {
        fprintf(stderr, "type error was [%s], wanted [%s]\n", program.vm.error, message);
    }
    LZ_CHECK(strstr(program.vm.error, message) != NULL);
    LZ_CHECK(strcmp(program.out.data != NULL ? program.out.data : "", expected) == 0);
    finish(&program);
}

static void check_help(const char *text, const char *message, const char *help) {
    Program program;
    run(&program, text, 0, 0);
    LZ_CHECK(program.compile_status == LZ_ERR_COMPILE);
    bool found = false;
    for (size_t i = 0; i < program.diagnostics.count; i++) {
        const LzDiagnostic *d = &program.diagnostics.items[i];
        if (strstr(d->message, message) != NULL && (help == NULL || strstr(d->extras.help, help) != NULL)) {
            found = true;
        }
    }
    if (!found) {
        fprintf(stderr, "program:\n%s\nexpected [%s] with help [%s]; got:\n", text, message, help != NULL ? help : "");
        for (size_t i = 0; i < program.diagnostics.count; i++) {
            fprintf(stderr, "  %s | help: %s\n", program.diagnostics.items[i].message,
                    program.diagnostics.items[i].extras.help);
        }
    }
    LZ_CHECK(found);
    finish(&program);
}

/* ---- 1. function types ---- */

static void test_function_type_syntax(void) {
    check_output("type Pred = fn(int) -> bool\nlet p: Pred = fn(n: int) -> bool { return n > 1 }\nprintln(p(3))\n", "true\n");
    check_output("fn apply(f: fn(int) -> int, x: int) -> int { return f(x) }\n"
                 "fn double(n: int) -> int { return n * 2 }\nprintln(apply(double, 4))\n", "8\n");
    check_output("fn run(f: fn(int)) { f(1) }\nrun(fn(n) => println(n))\n", "1\n");
    check_output("fn make() -> fn(int) -> int { return fn(n) => n + 1 }\nprintln(make()(1))\n", "2\n");
    check_output("let fs: (fn(int) -> int)[] = [fn(n) => n, fn(n) => n * 2]\nprintln(fs[1](4))\n", "8\n");
    check_output("let none: (fn(int) -> int)? = nil\nprintln(none)\n", "nil\n");
    check_output("let anyfn: fn = fn(a, b) => a\nprintln(anyfn(1, 2))\n", "1\n");
    check_output("class Button { pub on_click: fn(string) -> nil = fn(s) => nil }\n"
                 "let b = Button()\nb.on_click = fn(s) => println(s)\nb.on_click(\"hi\")\n", "hi\n");
    check_compile_error("let f: fn(int -> int = nil\n", "expected ')'");
    check_compile_error("let f: (fn(int) -> int) | fn(string) -> int = nil\n", "two different function types");
}

static void test_function_type_checks(void) {
    check_help("fn apply(f: fn(int) -> int, x: int) -> int { return f(x) }\n"
               "fn up(s: string) -> int { return 1 }\nprintln(apply(up, 4))\n",
               "expects fn(int) -> int, got fn(string) -> int", "change the parameter to int");
    check_help("fn two(a: int, b: int) -> int { return a + b }\nlet f: fn(int) -> int = two\n",
               "expected fn(int) -> int, got fn(int, int) -> int", "give the function 1 parameter");
    check_help("fn name(n: int) -> string { return \"x\" }\nlet f: fn(int) -> int = name\n",
               "expected fn(int) -> int, got fn(int) -> string", "return int from the function");
    check_help("let g: fn(int, string) -> bool = fn(a, b) => true\ng(1)\n",
               "this call has 1 argument", "pass 2 arguments");
    check_help("let g: fn(int) -> bool = fn(a) => true\ng(\"s\")\n", "argument 1 of this call expects int", NULL);
    check_compile_error("let g: fn(int) -> int = fn(a) => a\nlet s: string = g(1)\n", "expected string, got int");
    /* parameters are contravariant, results covariant */
    check_output("fn show(f: fn(int) -> number) { println(f(1)) }\nshow(fn(n: number) -> int { return 7 })\n", "7\n");
    check_compile_error("fn show(f: fn(number) -> int) { println(f(1)) }\nshow(fn(n: string) -> int { return 7 })\n", "expects fn(number) -> int");
}

static void test_function_type_run_time(void) {
    /* a value of unknown shape is checked when it arrives: only the count of arguments */
    check_output("fn run(cb: fn(int) -> int) { println(cb(1)) }\nlet h = fn(a) => a + 1\nrun(h)\n", "2\n");
    check_type_error("fn run(cb: fn(int) -> int) { println(cb(1)) }\nprintln(0)\nlet k = fn(a, b) => 1\nrun(k)\n",
                     "0\n", "expected fn(int) -> int, got function taking 2 arguments");
    check_type_error("let a = 5\nlet f: fn(int) -> int = a\n", "", "expected fn(int) -> int, got int");
    /* types are not tested at run time: a wrong-typed callback still runs */
    check_output("fn run(cb: fn(int) -> int) { println(cb(1)) }\nlet h = fn(a) => a\nrun(h)\n", "1\n");
    /* builtins and optional parameters fit by their arity */
    check_output("fn run(cb: fn(string) -> any) { println(cb(\"abc\")) }\nrun(len)\n", "3\n");
    check_output("fn run(cb: fn(int) -> int) { println(cb(1)) }\nfn dflt(a: int, b: int = 5) -> int { return a + b }\nrun(dflt)\n", "6\n");
}

/* The user-written map and filter the task asks for. */
static void test_callbacks(void) {
    check_output("fn map(xs: int[], f: fn(int) -> int) -> int[] {\n  let out: int[] = []\n  for x in xs { out.push(f(x)) }\n  return out\n}\n"
                 "fn filter(xs: int[], keep: fn(int) -> bool) -> int[] {\n  let out: int[] = []\n  for x in xs { if keep(x) { out.push(x) } }\n  return out\n}\n"
                 "println(map([1, 2, 3], fn(n) => n * n))\nprintln(filter([1, 2, 3, 4], fn(n) => n % 2 == 0))\n",
                 "[1, 4, 9]\n[2, 4]\n");
    check_help("fn filter(xs: int[], keep: fn(int) -> bool) -> int[] { return xs }\n"
               "fn size(s: string) -> bool { return true }\nprintln(filter([1], size))\n",
               "expects fn(int) -> bool", NULL);
}

/* ---- 2. generic functions ---- */

static void test_generic_declarations(void) {
    check_output("fn first<T>(xs: T[]) -> T { return xs[0] }\nprintln(first([7, 8]))\n", "7\n");
    check_output("fn pair<A, B>(a: A, b: B) -> A { return a }\nprintln(pair(1, \"x\"))\n", "1\n");
    /* inside the body T is any: it can annotate locals, and checks do not change */
    check_output("fn dup<T>(x: T[]) -> T[] { let y: T[] = x\n y.push(y[0])\n return y }\nprintln(dup([1]))\n", "[1, 1]\n");
    check_compile_error("fn f<>(x) { }\n", "expected a type parameter name");
    check_compile_error("fn f<T, T>(x: T) { }\n", "declared twice");
    check_compile_error("fn f<int>(x: int) { }\n", "builtin type");
    check_compile_error("fn f<T(x: T) { }\n", "expected '>'");
    check_compile_error("fn f(x: T) { }\n", "unknown type 'T'");
    /* a type parameter is only in scope in its function */
    check_compile_error("fn f<T>(x: T) { }\nlet y: T = 1\n", "unknown type 'T'");
}

static void test_generic_inference(void) {
    /* from an array: the result is int, so a string annotation is a compile error */
    check_compile_error("fn first<T>(xs: T[]) -> T { return xs[0] }\nlet s: string = first([1, 2])\n",
                        "expected string, got int");
    check_output("fn first<T>(xs: T[]) -> T { return xs[0] }\nlet n: int = first([1, 2])\nprintln(n)\n", "1\n");
    /* from a dict */
    check_compile_error("fn keys<K, V>(d: dict[K, V]) -> K[] { return [] }\nlet k: int[] = keys({\"a\": 1})\n",
                        "expected int[], got string[]");
    check_compile_error("fn vals<K, V>(d: dict[K, V]) -> V[] { return [] }\nlet k: string[] = vals({\"a\": 1})\n",
                        "expected string[], got int[]");
    /* from a function argument: U comes from the callback's declared result */
    check_compile_error("fn map<T, U>(xs: T[], f: fn(T) -> U) -> U[] { let o: U[] = []\n for x in xs { o.push(f(x)) }\n return o }\n"
                        "let r: int[] = map([1], fn(n: int) -> string { return \"s\" })\n", "expected int[], got string[]");
    check_output("fn map<T, U>(xs: T[], f: fn(T) -> U) -> U[] { let o: U[] = []\n for x in xs { o.push(f(x)) }\n return o }\n"
                 "let r: string[] = map([1, 2], fn(n: int) -> string { return string(n) })\nprintln(r)\n", "[\"1\", \"2\"]\n");
    /* from a class */
    check_compile_error("class P { pub x: int }\nfn id<T>(v: T) -> T { return v }\nlet a: P = P(1)\nlet i: int = id(a)\n", "expected int, got P");
    /* from a nullable parameter: T? takes the type without nil */
    check_compile_error("fn or_else<T>(x: T?, d: T) -> T { if x == nil { return d }\n return x }\nlet s: string = or_else(nil, 5)\n",
                        "expected string, got int");
    /* an argument that says nothing leaves T open: the result is any, no error */
    check_type_error("fn first<T>(xs: T[]) -> T { return xs[0] }\nlet a = [1]\nlet s: string = first(a)\n", "", "expected string");
    /* the callee's argument check uses what T became */
    check_compile_error("fn put<T>(xs: T[], x: T) { }\nput([1], \"s\")\n", "conflicting types for T");
}

static void test_generic_conflicts(void) {
    check_help("fn same<T>(a: T, b: T) -> T { return a }\nsame(1, \"a\")\n", "conflicting types for T in a call of same(): int and string",
               "int | string");
    check_help("fn same<T>(a: T, b: T) -> T { return a }\nsame(1, \"a\")\n", "int and string", "pass values of one type");
    /* int and float are two types too; number is the one type that holds both */
    check_compile_error("fn same<T>(a: T, b: T) -> T { return a }\nprintln(same(1, 2.5))\n", "int and float");
    check_output("fn same<T>(a: T, b: T) -> T { return a }\nlet n: number = 1\nprintln(same(n, 2.5))\n", "1\n");
    /* the suggested fix works */
    check_output("fn same(a: int | string, b: int | string) -> int | string { return a }\nprintln(same(1, \"a\"))\n", "1\n");
    /* values do not change at run time: an int[] passed through a generic stays an int[] */
    check_type_error("fn id<T>(x: T[]) -> T[] { return x }\nlet a: int[] = [1]\nid(a)\na.push(\"s\")\n", "", "array of int cannot hold string");
}

static void test_generics_as_methods(void) {
    check_output("extend array {\n pub fn head<T>(self: T[]) -> T { return self[0] }\n}\nprintln([4, 5].head())\n", "4\n");
    check_output("extend array {\n pub fn mapped<T, U>(self: T[], f: fn(T) -> U) -> U[] {\n  let out: U[] = []\n  for x in self { out.push(f(x)) }\n  return out\n }\n}\n"
                 "println([1, 2].mapped(fn(n: int) -> int { return n * 10 }))\n", "[10, 20]\n");
    check_output("class Box {\n pub item: any\n pub fn get<T>(self, fallback: T) -> T { return fallback }\n}\nprintln(Box(1).get(\"z\"))\n", "z\n");
    /* a free generic function used as a method by its first argument */
    check_compile_error("fn first<T>(xs: T[]) -> T { return xs[0] }\nlet n: string = first([3, 4])\n", "expected string, got int");
}

/* ---- 3. narrowing ---- */

/* Inside the branch the variable has the narrowed type, so `let s: string = x` is judged against it. */
static void test_narrowing_forms(void) {
    const char *decl = "let x: string? = \"abc\"\n";
    /* x != nil */
    check_output("let x: string? = \"abc\"\nif x != nil { let s: string = x\n println(s.len()) }\n", "3\n");
    check_compile_error("let x: int? = 1\nif x != nil { let s: string = x }\n", "expected string, got int");
    /* x == nil, with the else branch */
    check_compile_error("let x: int? = 1\nif x == nil { let s: int = x }\n", "expected int, got nil");
    check_compile_error("let x: int? = 1\nif x == nil { } else { let s: string = x }\n", "expected string, got int");
    /* nil on the left */
    check_compile_error("let x: int? = 1\nif nil != x { let s: string = x }\n", "expected string, got int");
    /* x is T, x is not T */
    check_compile_error("let x: int | string | nil = 1\nif x is int { let s: string = x }\n", "expected string, got int");
    check_compile_error("let x: int | string | nil = 1\nif x is not int { let s: int = x }\n", "expected int, got string | nil");
    check_compile_error("let x: int | string = 1\nif x is int { } else { let s: int = x }\n", "expected int, got string");
    /* the else-if chain narrows by what the earlier conditions ruled out */
    check_compile_error("let x: int | string | nil = 1\nif x is int { } else if x is string { let a: int = x }\n", "expected int, got string");
    check_compile_error("let x: int | string | nil = 1\nif x is int { } else if x is string { } else { let a: int = x }\n", "expected int, got nil");
    /* not (...) */
    check_compile_error("let x: int? = 1\nif !(x == nil) { let s: string = x }\n", "expected string, got int");
    check_compile_error("let x: int? = 1\nif !(x != nil) { let s: int = x }\n", "expected int, got nil");
    /* and: both sides count, and the right side already sees the left */
    check_output("let x: string? = \"hello\"\nif x != nil and x.len() > 2 { println(x) }\n", "hello\n");
    check_compile_error("let x: int? = 1\nlet y: int? = 2\nif x != nil and y != nil { let a: string = y }\n", "expected string, got int");
    check_compile_error("let x: int? = 1\nlet y: int? = 2\nif x != nil and y != nil { let a: string = x }\n", "expected string, got int");
    /* or: only when both sides say something about the same variable */
    check_compile_error("let x: int | string | nil = 1\nif x is int or x is string { let a: bool = x }\n", "expected bool, got int | string");
    check_output("let x: int | string | nil = nil\nif x == nil or x is string { println(\"yes\") }\n", "yes\n");
    /* x is a different variable from y: or tells nothing */
    check_output("let x: int? = nil\nlet y: int? = 1\nif x != nil or y != nil { let a: int? = x\n println(a) }\n", "nil\n");
    (void)decl;
}

static void test_narrowing_early_exit(void) {
    check_compile_error("fn f(x: int?) {\n  if x == nil { return }\n  let s: string = x\n}\n", "expected string, got int");
    check_output("fn f(x: string?) {\n  if x == nil { return }\n  let s: string = x\n  println(s.len())\n}\nf(\"abc\")\nf(nil)\n", "3\n");
    /* only when the block leaves: otherwise nothing is certain after the if */
    check_output("fn f(x: int?) {\n  if x == nil { println(\"n\") }\n  let s: int? = x\n  println(s)\n}\nf(nil)\n", "n\nnil\n");
    /* and the narrowing ends with the block that holds the if */
    check_output("fn f(x: int?, c: bool) {\n  if c {\n    if x == nil { return }\n  }\n  let s: int? = x\n  println(s)\n}\nf(nil, false)\n", "nil\n");
    check_compile_error("fn f(x: int | string) {\n  if x is string { return }\n  let s: string = x\n}\n", "expected string, got int");
    /* a chain whose every arm leaves keeps what the failed conditions ruled out */
    check_compile_error("fn f(x: int | string | nil) {\n  if x == nil { return } else if x is string { return }\n  let s: string = x\n}\n", "expected string, got int");
}

static void test_narrowing_is_cancelled(void) {
    /* an assignment ends it */
    check_type_error("let x: int? = 1\nif x != nil { x = nil\n let k: int = x }\n", "", "expected int, got nil");
    check_output("let x: int? = 1\nif x != nil { let a: int = x\n x = nil\n let b: string? = x\n println(b) }\n", "nil\n");
    /* compound assignment too, and so does one in a nested branch */
    check_output("let x: int? = 1\nif x != nil { if true { x = nil }\n let b: string? = x\n println(b) }\n", "nil\n");
    /* after the branch the old state is back, whatever happened inside */
    check_output("let x: int? = nil\nif x != nil { x = 5 }\nlet y: string? = x\nprintln(y)\n", "nil\n");
    /* a loop that assigns what was narrowed before it */
    check_type_error("let x: int? = 1\nif x != nil {\n  for i in 0..2 {\n    let t: nil = x\n    x = nil\n  }\n}\n", "", "expected nil, got int");
    /* while: the condition's narrowing holds in the body */
    check_compile_error("let x: int? = 1\nwhile x != nil { let s: string = x\n x = nil }\n", "expected string, got int");
}

static void test_narrowing_and_closures(void) {
    /* a function body assigns x anywhere: no narrowing at all */
    check_output("let x: int? = 1\nfn reset() { x = nil }\nif x != nil { reset()\n let s: string? = x\n println(s) }\n", "nil\n");
    check_output("let x: int? = 1\nif x != nil { let f = fn() { x = nil }\n f()\n let s: string? = x\n println(s) }\n", "nil\n");
    /* a closure reading x does not see the narrowing: it may run later */
    check_type_error("fn f(x: int?) {\n  if x != nil {\n    let g = fn() { let s: nil = x }\n    g()\n  }\n}\nf(2)\n", "", "expected nil, got int");
}

static void test_narrowing_leaves_old_programs_alone(void) {
    /* untyped values stay any: no error appears from a comparison with nil */
    check_output("let x = 5\nif x == nil { let s: string = x }\nprintln(\"ok\")\n", "ok\n");
    check_output("let x = 5\nif x != nil { x = \"s\" }\nprintln(x)\n", "s\n");
    /* fields and indexes are not narrowed */
    check_output("class P { pub n: int? = nil }\nlet p = P()\nif p.n != nil { println(1) } else { println(0) }\n", "0\n");
    check_output("let xs: (int | nil)[] = [nil]\nif xs[0] != nil { println(1) } else { println(0) }\n", "0\n");
    /* a nil method call that is not proven stays a run-time error, as it was */
    check_runtime_error("let n: string? = nil\nprintln(0)\nn.len()\n", "0\n", "nil has no method 'len'");
}

static void test_narrowing_nil_receiver(void) {
    check_help("fn f(name: string?) {\n  if name == nil {\n    name.len()\n  }\n}\n", "nil has no method 'len'", "check `name != nil` first");
    Program program;
    run(&program, "fn f(name: string?) {\n  if name == nil {\n    name.len()\n  }\n}\n", 0, 0);
    LZ_CHECK(program.diagnostics.count == 1);
    LZ_CHECK(program.diagnostics.items[0].extras.has_fix);
    LZ_CHECK(strcmp(program.diagnostics.items[0].extras.fix_text, "if name != nil { name.len() }") == 0);
    finish(&program);
}

/* ---- 4. inferred results ---- */

static void test_return_inference(void) {
    /* one return */
    check_compile_error("fn one() { return 1 }\nlet s: string = one()\n", "expected string, got int");
    check_output("fn one() { return 1 }\nlet n: int = one()\nprintln(n)\n", "1\n");
    /* several: the union */
    check_compile_error("fn pick(c) { if c { return 1 }\n return \"s\" }\nlet b: bool = pick(true)\n", "expected bool, got int | string");
    /* a path without a return gives nil too */
    check_compile_error("fn maybe(c) { if c { return 1 } }\nlet s: string = maybe(true)\n", "expected string, got int | nil");
    check_compile_error("fn nothing() { println(1) }\nlet n: int = nothing()\n", "expected int, got nil");
    /* a body that always returns has no nil */
    check_compile_error("fn both(c) { if c { return 1 } else { return 2 } }\nlet s: nil = both(true)\n", "expected nil, got int");
    /* expression bodies and lambdas */
    check_compile_error("let f = fn(x) => \"s\"\nfn id(x) => 5\nlet s: string = id(1)\n", "expected string, got int");
    /* recursion and unknown types make it any: no error, no new run-time check */
    check_output("fn fact(n) { if n <= 1 { return 1 }\n return n * fact(n - 1) }\nlet s: int = fact(3)\nprintln(s)\n", "6\n");
    check_output("fn unknown(x) { return x }\nlet s: string = unknown(\"a\")\nprintln(s)\n", "a\n");
    check_output("fn even(n) { if n == 0 { return true }\n return odd(n - 1) }\nfn odd(n) { if n == 0 { return false }\n return even(n - 1) }\nprintln(even(4))\n", "true\n");
    /* arithmetic on an untyped value might be an object with operators: not trusted */
    check_output("class V {\n pub n: int\n pub operator *(self, k) => V(self.n * k)\n}\nfn double(x) { return x * 2 }\nlet v: V = double(V(2))\nprintln(v.n)\n", "4\n");
    /* an inferred result checks nothing at run time: the function still returns what it returns */
    check_output("fn mixed(c) { if c { return 1 }\n return \"s\" }\nprintln(mixed(false))\n", "s\n");
    /* the inferred type reaches callbacks */
    check_compile_error("fn run(f: fn(int) -> string) { }\nfn n(x: int) { return 1 }\nrun(n)\n", "expects fn(int) -> string");
    /* a declared result still wins */
    check_output("fn f() -> number { return 1 }\nlet n: number = f()\nprintln(n)\n", "1\n");
}

int main(void) {
    test_function_type_syntax();
    test_function_type_checks();
    test_function_type_run_time();
    test_callbacks();
    test_generic_declarations();
    test_generic_inference();
    test_generic_conflicts();
    test_generics_as_methods();
    test_narrowing_forms();
    test_narrowing_early_exit();
    test_narrowing_is_cancelled();
    test_narrowing_and_closures();
    test_narrowing_leaves_old_programs_alone();
    test_narrowing_nil_receiver();
    test_return_inference();
    return LZ_TEST_RESULT();
}
