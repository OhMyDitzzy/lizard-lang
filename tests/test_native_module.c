/*
 * Native modules through lzapi.h. The module under test is tests/native/modlib.c, included first so
 * that it sees nothing but lzapi.h. The Lizard programs below run on whichever engine the variant
 * of this test selects, and the expected output is the same on all of them.
 */
#include "native/modlib.c"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lizard/builtin.h"
#include "lizard/compiler.h"
#include "lizard/link.h"
#include "lizard/module.h"
#include "lizard/native_module.h"
#include "test_util.h"

typedef struct Src {
    const char *name;
    const char *text;
} Src;

typedef struct Env {
    LzVM vm;
    LzBuffer out;
    LzBuffer err;
    const Src *sources;
    size_t source_count;
    LzChunk chunk;
    LzDiagnostics diagnostics;
    LzSource main_source;
    bool has_chunk;
} Env;

static char *copy_text(const char *text) {
    size_t length = strlen(text);
    char *copy = malloc(length + 1);
    memcpy(copy, text, length + 1);
    return copy;
}

static LzStatus resolve(void *context, LzVM *vm, const LzModule *importer, const char *name, LzModuleSource *out) {
    (void)importer;
    Env *env = context;
    for (size_t i = 0; i < env->source_count; i++) {
        if (strcmp(env->sources[i].name, name) == 0) {
            char file[64];
            (void)snprintf(file, sizeof file, "%s.lz", name);
            out->id = copy_text(file);
            out->name = copy_text(file);
            out->text = copy_text(env->sources[i].text);
            out->length = strlen(out->text);
            return LZ_OK;
        }
    }
    return lz_vm_set_error(vm, LZ_ERR_RUNTIME, "cannot find module '%s'", name);
}

static void setup(Env *env, const Src *sources, size_t count) {
    memset(env, 0, sizeof *env);
    nmod_reset();
    lz_vm_init(&env->vm, lz_writer_from_buffer(&env->out), lz_writer_from_buffer(&env->err));
    LZ_CHECK(lz_builtin_install(&env->vm) == LZ_OK);
    env->sources = sources;
    env->source_count = count;
    LzModuleResolver resolver = {env, resolve, NULL};
    lz_vm_set_resolver(&env->vm, &resolver);
    LZ_CHECK(lz_vm_register_native_module(&env->vm, "test.nmod", nmod_init) == LZ_OK);
    LZ_CHECK(lz_vm_register_native_module(&env->vm, "test.other", other_init) == LZ_OK);
}

static LzStatus run_main(Env *env, const char *text) {
    lz_source_from_string(&env->main_source, "main.lz", text);
    env->vm.trace_source = &env->main_source;
    lz_chunk_init(&env->chunk);
    env->has_chunk = true;
    LzStatus status = lz_compile(&env->vm, &env->main_source, &env->chunk, &env->diagnostics);
    if (status != LZ_OK) {
        return status;
    }
    return lz_vm_run(&env->vm, &env->chunk);
}

static const char *out_of(const Env *env) {
    return env->out.data != NULL ? env->out.data : "";
}

static void finish(Env *env, bool clean) {
    if (env->has_chunk) {
        lz_chunk_free(&env->vm.heap, &env->chunk);
    }
    lz_vm_release_roots(&env->vm);
    if (clean) {
        LZ_CHECK(env->vm.heap.live_objects == 0);
        LZ_CHECK(env->vm.heap.live_bytes == 0);
    }
    lz_vm_free(&env->vm);
    lz_buffer_free(&env->out);
    lz_buffer_free(&env->err);
}

/* Runs a program that has to succeed and compares what it printed. */
static void run_expect(const char *program, const char *expected) {
    Env env;
    setup(&env, NULL, 0);
    LzStatus status = run_main(&env, program);
    if (status != LZ_OK || strcmp(out_of(&env), expected) != 0) {
        fprintf(stderr, "---- program\n%s\n---- expected\n%s\n---- got (status %d)\n%s\n---- error: %s\n", program, expected,
                (int)status, out_of(&env),
                env.diagnostics.count > 0 ? env.diagnostics.items[0].message : env.vm.error);
        LZ_CHECK(false);
    }
    finish(&env, true);
}

/* Runs a program that has to fail to compile with a message that holds `needle`. */
static void compile_fails(const char *program, const char *needle) {
    Env env;
    setup(&env, NULL, 0);
    LzStatus status = run_main(&env, program);
    bool ok = status == LZ_ERR_COMPILE && env.diagnostics.count > 0 && strstr(env.diagnostics.items[0].message, needle) != NULL;
    if (!ok) {
        fprintf(stderr, "expected a compile error with [%s], got status %d [%s]\n", needle, (int)status,
                env.diagnostics.count > 0 ? env.diagnostics.items[0].message : "");
        LZ_CHECK(false);
    }
    finish(&env, false);
}

/* Runs a program that has to stop at run time with an error that holds `needle`. */
static void run_fails(const char *program, LzStatus want, const char *needle) {
    Env env;
    setup(&env, NULL, 0);
    LzStatus status = run_main(&env, program);
    bool ok = status == want && strstr(env.vm.error, needle) != NULL;
    if (!ok) {
        fprintf(stderr, "expected status %d with [%s], got %d [%s]\n%s\n", (int)want, needle, (int)status, env.vm.error,
                out_of(&env));
        LZ_CHECK(false);
    }
    finish(&env, false);
}

static void test_values(void) {
    run_expect("import test.nmod as n\n"
               "println(n.add(2, 3))\n"
               "println(n.scale(1.5, 4))\n"
               "println(n.greet(\"lizard\"))\n"
               "println(n.flip(true))\n"
               "println(n.nothing())\n"
               "println(n.kind(1), n.kind(1.5), n.kind(\"s\"), n.kind(nil), n.kind(true), n.kind([1]), n.kind({}))\n"
               "println(n.count_args(), n.count_args(1), n.count_args(1, 2, 3))\n"
               "println(n.strict(1), n.strict(1.5), n.strict(true), n.strict(\"s\"), n.strict(nil))\n"
               "println(n.VERSION, n.HALF, n.NAME, n.ON)\n",
               "5\n6.0\nhello, lizard\nfalse\nnil\nint float string nil bool array dict\n3 3 3\n1 10 100 1000 0\n"
               "3 0.5 nmod true\n");
}

static void test_arrays_and_dicts(void) {
    run_expect("import test.nmod as n\n"
               "println(n.sum([1, 2, 3, 4]))\n"
               "println(n.range(5))\n"
               "let xs = [1, 2, 3]\n"
               "n.poke(xs, 1, 40)\n"
               "println(xs)\n"
               "let d = n.make_dict(\"a\", 9)\n"
               "println(d)\n"
               "println(n.dict_info(d))\n"
               "println(n.dict_info({\"a\": 2}))\n"
               "try {\n"
               "    n.poke(xs, 10, 1)\n"
               "} catch RangeError as e {\n"
               "    println(\"range:\", e.message)\n"
               "}\n"
               "let mixed: any = [1, \"x\"]\n"
               "try {\n"
               "    n.sum(mixed)\n"
               "} catch TypeError as e {\n"
               "    println(\"type:\", e.message)\n"
               "}\n",
               "10\n[0, 1, 4, 9, 16]\n[1, 40, 3]\n{\"a\": 9, 7: \"seven\"}\n"
               "109112\n102011\nrange: test.nmod.poke: array index 10 out of range\ntype: sum: item 1 is not an int\n");
}

static void test_errors(void) {
    run_expect("import test.nmod as n\n"
               "import test.other as o\n"
               "try {\n    n.fail(0)\n} catch TypeError as e {\n    println(\"a\", e.message)\n}\n"
               "try {\n    n.fail(1)\n} catch RangeError as e {\n    println(\"b\", e.message)\n}\n"
               "try {\n    n.fail(2)\n} catch n.NmodError as e {\n    println(\"c\", e.message)\n}\n"
               "try {\n    n.fail(3)\n} catch n.NmodError as e {\n    println(\"d\", e.message)\n}\n"
               "try {\n    n.fail(3)\n} catch n.NmodChild as e {\n    println(\"e\", e.message)\n}\n"
               "try {\n    n.fail(2)\n} catch RuntimeError as e {\n    println(\"f\", e.message)\n}\n"
               "try {\n    o.fail()\n} catch o.NmodError as e {\n    println(\"g\", e.message)\n}\n"
               "try {\n    try {\n        o.fail()\n    } catch n.NmodError as e {\n        println(\"wrong\")\n    }\n"
               "} catch o.NmodError as e {\n    println(\"h\", e.message)\n}\n"
               "try {\n    n.fail(9)\n} catch RuntimeError as e {\n    println(\"i\", e.message)\n}\n"
               "let made = n.NmodError(\"from lizard\")\n"
               "try {\n    throw made\n} catch n.NmodError as e {\n    println(\"j\", e.message)\n}\n",
               "a builtin type error 1\nb builtin range error\nc module error\nd child of the module error\n"
               "e child of the module error\nf module error\ng the other module failed\nh the other module failed\n"
               "i test.nmod.fail(): failed without an error\nj from lizard\n");
}

static void test_argument_checks(void) {
    run_expect("import test.nmod as n\n"
               "let v: any = \"x\"\n"
               "try {\n    n.add(1, v)\n} catch TypeError as e {\n    println(e.message)\n}\n"
               "let f: any = 1.5\n"
               "try {\n    n.add(f, 1)\n} catch TypeError as e {\n    println(e.message)\n}\n"
               "let k: any = 2\n"
               "println(n.scale(k, 2))\n"
               "try {\n    n.greet(k)\n} catch TypeError as e {\n    println(e.message)\n}\n"
               "let two: any = 2\n"
               "try {\n    n.sum(two)\n} catch TypeError as e {\n    println(e.message)\n}\n",
               "argument 'b' of test.nmod.add() expects int, got \"x\"\nargument 'a' of test.nmod.add() expects int, got float\n4.0\n"
               "argument 'name' of test.nmod.greet() expects string, got int\nargument 'values' of test.nmod.sum() expects array, got int\n");
    run_fails("import test.nmod as n\nlet a: any = []\nlet f: any = n.add\nf(a)\n", LZ_ERR_ARITY, "expected 2 arguments, got 1");
    run_fails("import test.nmod as n\nlet a: any = []\nlet f: any = n.count_args\nf(a, a, a, a)\n", LZ_ERR_ARITY,
              "expected 0 to 3 arguments, got 4");
    compile_fails("import test.nmod as n\nn.add(1, \"x\")\n", "type mismatch");
    compile_fails("import test.nmod as n\nlet s: string = n.add(1, 2)\n", "int");
    compile_fails("import test.nmod as n\nn.greet(1)\n", "type mismatch");
}

static void test_import_forms(void) {
    run_expect("from test.nmod import add, greet as hi, VERSION\n"
               "println(add(1, 1), hi(\"x\"), VERSION)\n",
               "2 hello, x 3\n");
    const Src sources[] = {{"left", "import test.nmod as n\npub fn left(x: int) => n.add(x, 1)\n"},
                           {"right", "import test.nmod as m\npub fn right(x: int) => m.add(x, 2)\n"}};
    Env env;
    setup(&env, sources, 2);
    LzStatus status = run_main(&env, "import left\nimport right\nimport test.nmod\nprintln(left.left(1), right.right(1), nmod.add(5, 5))\n");
    if (status != LZ_OK) {
        fprintf(stderr, "import forms: %s\n", env.diagnostics.count > 0 ? env.diagnostics.items[0].message : env.vm.error);
    }
    LZ_CHECK(status == LZ_OK);
    LZ_CHECK(strcmp(out_of(&env), "2 3 10\n") == 0);
    LZ_CHECK(nmod_inits() == 1);
    finish(&env, true);
}

static void test_class(void) {
    run_expect("import test.nmod as n\n"
               "let c = n.Counter(5)\n"
               "println(c.add(), c.add(10), c.value)\n"
               "let d = n.Counter()\n"
               "println(d.add(), d.value)\n"
               "let e = n.Counter(1, 10)\n"
               "println(e.add(), e.add(2))\n"
               "println(c is n.Counter, 1 is n.Counter, typeof(c))\n"
               "fn show(x: n.Counter) => x.value\n"
               "println(show(c), show(d))\n"
               "let m = c.merge(d)\n"
               "println(c.value, m.value)\n"
               "println(c.apply_to(fn(v: int) => v * 2))\n"
               "let g = c.add\n"
               "println(g(1))\n"
               "println(n.created())\n",
               "6 16 16\n1 1\n11 13\ntrue false Counter\n16 1\n1016 1\n2032\n1017\n3\n");
    run_fails("import test.nmod as n\nn.Counter(\"x\")\n", LZ_ERR_TYPE, "argument 'value' of Counter.init() expects int, got");
    run_fails("import test.nmod as n\nn.Counter(1, 2, 3)\n", LZ_ERR_ARITY, "expected 0 to 2 arguments, got 3");
    run_fails("import test.nmod as n\nlet c = n.Counter(1)\nc.add(\"x\")\n", LZ_ERR_TYPE, "argument 'n' of Counter.add() expects int, got");
    run_fails("import test.nmod as n\nlet c = n.Counter(1)\nlet d = c.copy()\n", LZ_ERR_RUNTIME, "cannot copy an instance of the native class");
    run_fails("import test.nmod as n\nclass Mine : n.Counter {\n}\n", LZ_ERR_RUNTIME, "cannot extend a native class");
}

static void test_finalizers(void) {
    Env env;
    setup(&env, NULL, 0);
    LzStatus status = run_main(&env, "import test.nmod as n\n"
                                      "let a = n.Counter(1)\n"
                                      "let b = n.Counter(2)\n"
                                      "{\n    let tmp = n.Counter(3)\n    println(tmp.value)\n}\n"
                                      "try {\n    n.Counter(-1)\n} catch n.NmodError as e {\n    println(e.message)\n}\n"
                                      "println(n.created(), n.finalized())\n"
                                      "n.keep(n.Counter(9))\n"
                                      "println(n.created(), n.finalized())\n");
    LZ_CHECK(status == LZ_OK);
    LZ_CHECK(strcmp(out_of(&env), "3\nCounter starts at a negative value\n4 2\n5 2\n") == 0 || strstr(out_of(&env), "4 ") != NULL);
    if (strcmp(out_of(&env), "3\nCounter starts at a negative value\n4 2\n5 2\n") != 0) {
        fprintf(stderr, "finalizer output: [%s]\n", out_of(&env));
        LZ_CHECK(false);
    }
    /* a, b and the kept one are alive: the VM finalizes each exactly once when it is freed */
    LZ_CHECK(nmod_created() == 5);
    lz_chunk_free(&env.vm.heap, &env.chunk);
    env.has_chunk = false;
    lz_vm_free(&env.vm);
    LZ_CHECK(nmod_finalized() == 5);
    lz_buffer_free(&env.out);
    lz_buffer_free(&env.err);
}

static void test_callbacks(void) {
    run_expect("import test.nmod as n\n"
               "println(n.apply(fn(x: int) => x + 1, 41))\n"
               "let base = 100\n"
               "println(n.apply(fn(x: int) => x + base, 1))\n"
               "try {\n    n.apply(fn(x: int) -> int => x % 0, 1)\n} catch ArithmeticError as e {\n    println(\"caught\", e.message)\n}\n"
               "fn boom(x: int) -> int {\n    n.fail(2)\n    return 0\n}\n"
               "try {\n    n.apply(boom, 1)\n} catch n.NmodError as e {\n    println(\"module\", e.message)\n}\n"
               "fn make(k: int) {\n    return fn(x: int) => x * k\n}\nn.store(make(3))\n"
               "println(n.fire(5), n.fire(6))\n"
               "n.clear()\n"
               "try {\n    n.fire(1)\n} catch RuntimeError as e {\n    println(e.message)\n}\n",
               "42\n101\ncaught division by zero\nmodule module error\n15 18\nno callback stored\n");
}

static void test_limits(void) {
    Env env;
    setup(&env, NULL, 0);
    lz_vm_set_fuel(&env.vm, 200);
    LzStatus status = run_main(&env, "import test.nmod as n\nn.spin(100000)\n");
    LZ_CHECK(status == LZ_ERR_FUEL);
    LZ_CHECK(strstr(env.vm.error, "instruction limit exceeded") != NULL);
    finish(&env, false);

    setup(&env, NULL, 0);
    lz_vm_set_fuel(&env.vm, 300);
    status = run_main(&env, "import test.nmod as n\nfn spin(x: int) {\n    while true {\n    }\n    return x\n}\nn.apply(spin, 1)\n");
    LZ_CHECK(status == LZ_ERR_FUEL);
    finish(&env, false);

    setup(&env, NULL, 0);
    status = run_main(&env, "import test.nmod as n\nprintln(n.spin(50))\n");
    LZ_CHECK(status == LZ_OK && strcmp(out_of(&env), "50\n") == 0);
    finish(&env, true);

    setup(&env, NULL, 0);
    lz_vm_set_heap_limit(&env.vm, 200000);
    status = run_main(&env, "import test.nmod as n\nprintln(n.alloc(1000))\nprintln(n.alloc(10000000))\n");
    if (status != LZ_ERR_HEAP_LIMIT) {
        fprintf(stderr, "heap: status %d [%s] out [%s]\n", (int)status, env.vm.error, out_of(&env));
    }
    LZ_CHECK(status == LZ_ERR_HEAP_LIMIT);
    LZ_CHECK(strstr(env.vm.error, "heap limit") != NULL);
    LZ_CHECK(strcmp(out_of(&env), "1000\n") == 0);
    finish(&env, false);

    setup(&env, NULL, 0);
    status = run_main(&env, "import test.nmod as n\nn.remember_handle()\nprintln(n.use_handle())\n");
    LZ_CHECK(status == LZ_ERR_RUNTIME);
    LZ_CHECK(strstr(env.vm.error, "stale or invalid native handle") != NULL);
    finish(&env, false);
}

static void test_signature_defaults(void) {
    Env env;
    setup(&env, NULL, 0);
    LZ_CHECK(lz_vm_register_native_module(&env.vm, "test.defaults", defaults_test_init) == LZ_OK);
    LzStatus status = run_main(&env, "import test.defaults as d\nprintln(d.values(1), d.values(1, 2), d.values(1, 2, 3), d.legacy(4, 5), d.literals())\n");
    LZ_CHECK(status == LZ_OK);
    LZ_CHECK(strcmp(out_of(&env), "51 33 6 9 1\n") == 0);
    lz_native_vm_shutdown(&env.vm);
    finish(&env, true);
}

static void test_registration(void) {
    Env env;
    setup(&env, NULL, 0);
    LZ_CHECK(lz_vm_register_native_module(&env.vm, "test.nmod", nmod_init) == LZ_ERR_RUNTIME);
    LZ_CHECK(strstr(env.vm.error, "registered twice") != NULL);
    LZ_CHECK(lz_vm_register_native_module(&env.vm, "bad name", nmod_init) == LZ_ERR_RUNTIME);
    LZ_CHECK(lz_vm_register_native_module(&env.vm, "test.badsig", badsig_init) == LZ_ERR_RUNTIME);
    LZ_CHECK(strstr(env.vm.error, "invalid signature for 'f'") != NULL && strstr(env.vm.error, "missing ')'") != NULL);
    LZ_CHECK(lz_vm_register_native_module(&env.vm, "test.badtype", badtype_init) == LZ_ERR_RUNTIME);
    LZ_CHECK(strstr(env.vm.error, "invalid signature for 'f'") != NULL);
    LZ_CHECK(lz_vm_register_native_module(&env.vm, "test.badcount", badcount_init) == LZ_ERR_RUNTIME);
    LZ_CHECK(strstr(env.vm.error, "a parameter without a default cannot follow one with a default") != NULL);
    LZ_CHECK(lz_vm_register_native_module(&env.vm, "test.duplicate_param", duplicate_param_init) == LZ_ERR_RUNTIME);
    LZ_CHECK(strstr(env.vm.error, "duplicate parameter name 'a'") != NULL);
    LZ_CHECK(lz_vm_register_native_module(&env.vm, "test.dup", dupname_init) == LZ_ERR_RUNTIME);
    LZ_CHECK(strstr(env.vm.error, "declares 'f' twice") != NULL);
    const char *invalid[] = {
        "fn(a: int = 1 + 2)",
        "fn(a: int = 0, b: int)",
        "fn(*a: any, *b: any)",
        "fn(*a: any, b: int)",
        "fn(a: int, a: int)",
        "fn(*a: any = nil)",
        "fn(a: bool = -true)",
    };
    const char *reasons[] = {
        "a parameter default must be a literal",
        "a parameter without a default cannot follow one with a default",
        "the rest parameter must be the last one",
        "the rest parameter must be the last one",
        "duplicate parameter name 'a'",
        "a rest parameter cannot have a default",
        "a parameter default must be a literal",
    };
    for (size_t i = 0; i < sizeof invalid / sizeof invalid[0]; i++) {
        invalid_native_signature = invalid[i];
        LZ_CHECK(lz_vm_register_native_module(&env.vm, "test.invalid", invalid_native_init) == LZ_ERR_RUNTIME);
        LZ_CHECK(strstr(env.vm.error, "cannot register native module 'test.invalid':") != NULL);
        LZ_CHECK(strstr(env.vm.error, reasons[i]) != NULL);
    }
    finish(&env, true);

    /* a failed registration is not an importable module */
    setup(&env, NULL, 0);
    LZ_CHECK(lz_vm_register_native_module(&env.vm, "test.badsig", badsig_init) == LZ_ERR_RUNTIME);
    LzStatus status = run_main(&env, "import test.badsig\n");
    LZ_CHECK(status == LZ_ERR_COMPILE);
    finish(&env, false);
}

static void test_std_prefix(void) {
    Env env;
    setup(&env, NULL, 0);
    LZ_CHECK(lz_vm_register_native_module(&env.vm, "std.nmod", other_init) == LZ_OK);
    LzStatus status = run_main(&env, "import std.nmod as s\ntry {\n    s.fail()\n} catch s.NmodError as e {\n    println(e.message)\n}\n");
    if (status != LZ_OK) {
        fprintf(stderr, "std prefix: %s\n", env.diagnostics.count > 0 ? env.diagnostics.items[0].message : env.vm.error);
    }
    LZ_CHECK(status == LZ_OK);
    finish(&env, true);
}

int main(void) {
    run_expect("import test.nmod as n\n"
               "println(n.rest_args(), n.rest_args(1), n.rest_args(1, 2, 3))\n"
               "println(n.mixed_args(), n.mixed_args(1), n.mixed_args(1, 2, 3))\n"
               "println(n.count_args(1), n.count_args(1, 2), n.count_args(1, 2, 3))\n",
               "0 1 3\n1 1 3\n3 3 3\n");
    test_signature_defaults();
    test_values();
    test_arrays_and_dicts();
    test_errors();
    test_argument_checks();
    test_import_forms();
    test_class();
    test_finalizers();
    test_callbacks();
    test_limits();
    test_registration();
    test_std_prefix();
    return LZ_TEST_RESULT();
}
