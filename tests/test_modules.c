#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lizard/builtin.h"
#include "lizard/class.h"
#include "lizard/compiler.h"
#include "lizard/link.h"
#include "lizard/module.h"
#include "test_util.h"

typedef struct Src {
    const char *name; /* the module name the resolver is asked for, also the id */
    const char *text;
} Src;

typedef struct Env {
    LzVM vm;
    LzBuffer out;
    LzBuffer err;
    Src *sources;
    size_t source_count;
    int resolves;
    int destroyed;
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

static LzStatus resolve(void *context, LzVM *vm, const LzModule *importer, const char *name,
                        LzModuleSource *out) {
    (void)importer;
    Env *env = context;
    env->resolves++;
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

static void destroy(void *context) {
    ((Env *)context)->destroyed++;
}

static void setup(Env *env, Src *sources, size_t count, bool with_resolver) {
    memset(env, 0, sizeof *env);
    lz_vm_init(&env->vm, lz_writer_from_buffer(&env->out), lz_writer_from_buffer(&env->err));
    LZ_CHECK(lz_builtin_install(&env->vm) == LZ_OK);
    env->sources = sources;
    env->source_count = count;
    if (with_resolver) {
        LzModuleResolver resolver = {env, resolve, destroy};
        lz_vm_set_resolver(&env->vm, &resolver);
    }
}

/* Gives a module's global to main under another name, as a host would. */
static void export_global(Env *env, LzModule *module, const char *name, const char *alias) {
    LzString *key = lz_vm_intern(&env->vm, name, strlen(name));
    const LzValue *value = lz_module_get(module, key);
    LZ_CHECK(value != NULL);
    if (value != NULL) {
        LzString *to = lz_vm_intern(&env->vm, alias, strlen(alias));
        LZ_CHECK(lz_vm_define_global(&env->vm, to, *value));
    }
}

static LzModule *load(Env *env, const char *name) {
    LzModule *module = NULL;
    LzStatus status = lz_module_load(&env->vm, NULL, name, &module);
    if (status != LZ_OK) {
        fprintf(stderr, "load %s: %s\n", name, env->vm.error);
    }
    LZ_CHECK(status == LZ_OK);
    return module;
}

static LzStatus run_main(Env *env, const char *text) {
    lz_source_from_string(&env->main_source, "main.lz", text);
    env->vm.trace_source = &env->main_source;
    lz_chunk_init(&env->chunk);
    env->has_chunk = true;
    LzStatus status = lz_compile(&env->vm, &env->main_source, &env->chunk, &env->diagnostics);
    if (status != LZ_OK) {
        fprintf(stderr, "main did not compile: %s\n", env->diagnostics.items[0].message);
        LZ_CHECK(false);
        return status;
    }
    return lz_vm_run(&env->vm, &env->chunk);
}

static void expect_out(Env *env, const char *expected) {
    const char *actual = env->out.data != NULL ? env->out.data : "";
    if (strcmp(actual, expected) != 0) {
        fprintf(stderr, "expected [%s] got [%s] (error: %s)\n", expected, actual, env->vm.error);
    }
    LZ_CHECK(strcmp(actual, expected) == 0);
}

static void finish(Env *env) {
    LZ_CHECK(env->vm.stack_count == 0);
    if (env->has_chunk) {
        lz_chunk_free(&env->vm.heap, &env->chunk);
    }
    lz_vm_release_roots(&env->vm);
    LZ_CHECK(env->vm.heap.live_objects == 0);
    LZ_CHECK(env->vm.heap.live_bytes == 0);
    LZ_CHECK(lz_link_check(&env->vm));
    lz_vm_free(&env->vm);
    LZ_CHECK(env->destroyed <= 1);
    lz_buffer_free(&env->out);
    lz_buffer_free(&env->err);
}

static void test_separate_globals(void) {
    Src sources[] = {{"a", "let x = 1\nfn getx() => x\nfn bump() {\n    x = x + 1\n    return x\n}\n"},
                     {"b", "let x = 2\nfn getx() => x\n"}};
    Env env;
    setup(&env, sources, 2, true);
    LzModule *a = load(&env, "a");
    LzModule *b = load(&env, "b");
    LZ_CHECK(a != NULL && b != NULL && a != b);
    export_global(&env, a, "getx", "ga");
    export_global(&env, a, "bump", "bump");
    export_global(&env, b, "getx", "gb");
    LZ_CHECK(run_main(&env, "let x = 9\nprintln(x, ga(), gb())\nprintln(bump(), x, ga(), gb())\n") == LZ_OK);
    expect_out(&env, "9 1 2\n2 9 2 2\n");
    finish(&env);
}

static void test_function_uses_its_module(void) {
    Src sources[] = {{"a", "let base = 10\nfn add(n) => n + base\nfn make() {\n    return fn(n) => n + base\n}\n"}};
    Env env;
    setup(&env, sources, 1, true);
    LzModule *a = load(&env, "a");
    export_global(&env, a, "add", "add");
    export_global(&env, a, "make", "make");
    LZ_CHECK(run_main(&env,
                      "let base = 1000\nprintln(add(1))\nlet c = make()\nprintln(c(2))\n"
                      "println([1, 2].map(add))\nfn local(n) => n + base\nprintln([1].map(local))\n") == LZ_OK);
    expect_out(&env, "11\n12\n[11, 12]\n[1001]\n");
    finish(&env);
}

static void test_loaded_once(void) {
    Src sources[] = {{"a", "println(\"top level\")\nlet x = 1\n"}};
    Env env;
    setup(&env, sources, 1, true);
    LzModule *first = load(&env, "a");
    LzModule *second = load(&env, "a");
    LZ_CHECK(first == second);
    LZ_CHECK(env.vm.module_count == 2); /* main and a */
    expect_out(&env, "top level\n");
    finish(&env);
}

static void test_circular(void) {
    Src sources[] = {{"a", "import b\n"}, {"b", "import a\n"}};
    Env env;
    setup(&env, sources, 2, true);
    LzModule *module = NULL;
    LZ_CHECK(lz_module_load(&env.vm, NULL, "a", &module) != LZ_OK);
    LZ_CHECK(strstr(env.vm.error, "circular import of 'a'") != NULL);
    LZ_CHECK(lz_vm_module_failure(&env.vm) != NULL);
    LZ_CHECK(strstr(lz_vm_module_failure(&env.vm)->next->diagnostics.items[0].extras.note, "a.lz -> b.lz -> a.lz") != NULL);
    LZ_CHECK(env.vm.module_count == 1); /* nothing half loaded stays in the list */
    LZ_CHECK(lz_module_load(&env.vm, NULL, "a", &module) != LZ_OK); /* not cached, tried again */
    LZ_CHECK(env.resolves >= 4);
    finish(&env);
}

static void test_nested_load_and_trace(void) {
    Src sources[] = {{"a", "let v = 1\nfn boom(n) {\n    return n / 0\n}\n"},
                     {"b", "import a\nlet z = 0\nlet y = 1 / z\n"}};
    Env env;
    setup(&env, sources, 2, true);
    LzModule *a = load(&env, "a");
    export_global(&env, a, "boom", "boom");
    LZ_CHECK(run_main(&env, "fn outer() => boom(1)\nouter()\n") != LZ_OK);
    LZ_CHECK(env.vm.error_source != NULL && strcmp(env.vm.error_source->name, "a.lz") == 0);
    bool saw_a = false;
    bool saw_main = false;
    for (size_t i = 0; i < env.vm.trace_count; i++) {
        const LzTraceEntry *entry = &env.vm.trace[i];
        if (entry->source != NULL && strcmp(entry->source->name, "a.lz") == 0) {
            saw_a = true;
        }
        if (entry->source == NULL) {
            saw_main = true;
        }
    }
    LZ_CHECK(saw_a && saw_main);
    LzModule *failed = NULL;
    lz_chunk_free(&env.vm.heap, &env.chunk);
    env.has_chunk = false;
    LZ_CHECK(lz_module_load(&env.vm, NULL, "b", &failed) != LZ_OK);
    LZ_CHECK(failed == NULL);
    LZ_CHECK(env.vm.error_source != NULL && strcmp(env.vm.error_source->name, "b.lz") == 0);
    LZ_CHECK(env.vm.module_count == 2); /* main and a: b failed and is not cached */
    finish(&env);
}

static void test_failures(void) {
    Src sources[] = {{"bad", "let x = \nfn ( {\n"}};
    Env env;
    LzModule *module = NULL;
    setup(&env, sources, 1, false);
    LZ_CHECK(lz_module_load(&env.vm, NULL, "bad", &module) != LZ_OK);
    LZ_CHECK(strstr(env.vm.error, "modules are not available here") != NULL);
    finish(&env);

    setup(&env, sources, 1, true);
    LZ_CHECK(lz_module_load(&env.vm, NULL, "missing", &module) != LZ_OK);
    LZ_CHECK(strstr(env.vm.error, "cannot find module 'missing'") != NULL);
    LZ_CHECK(lz_module_load(&env.vm, NULL, "bad", &module) != LZ_OK);
    LZ_CHECK(strstr(env.vm.error, "cannot load module") != NULL);
    LZ_CHECK(strstr(env.vm.error, "bad.lz:") != NULL);
    const LzModuleFailure *failure = lz_vm_module_failure(&env.vm);
    LZ_CHECK(failure != NULL && failure->diagnostics.count > 0);
    LZ_CHECK(env.vm.module_count == 1);
    finish(&env);
}

static void test_limits(void) {
    Src sources[] = {{"spin", "while true {\n}\n"},
                     {"fat", "let xs = []\nlet i = 0\nwhile i < 100000 {\n    xs.push(i)\n    i = i + 1\n}\n"}};
    Env env;
    LzModule *module = NULL;
    setup(&env, sources, 2, true);
    lz_vm_set_fuel(&env.vm, 5000);
    LZ_CHECK(lz_module_load(&env.vm, NULL, "spin", &module) == LZ_ERR_FUEL);
    finish(&env);
    setup(&env, sources, 2, true);
    lz_vm_set_heap_limit(&env.vm, 64 * 1024);
    LzStatus fat = lz_module_load(&env.vm, NULL, "fat", &module);
    if (fat != LZ_ERR_HEAP_LIMIT) {
        fprintf(stderr, "fat: status %d: %s\n", (int)fat, env.vm.error);
    }
    LZ_CHECK(fat == LZ_ERR_HEAP_LIMIT);
    lz_vm_set_heap_limit(&env.vm, LZ_HEAP_UNLIMITED);
    finish(&env);
}



static LzStatus resolve_many(void *context, LzVM *vm, const LzModule *importer, const char *name,
                             LzModuleSource *out) {
    (void)context;
    (void)importer;
    (void)vm;
    out->id = copy_text(name);
    out->name = copy_text(name);
    out->text = copy_text("let x = 1\n");
    out->length = strlen(out->text);
    return LZ_OK;
}

static void test_module_limit(void) {
    Env env;
    setup(&env, NULL, 0, false);
    LzModuleResolver resolver = {NULL, resolve_many, NULL};
    lz_vm_set_resolver(&env.vm, &resolver);
    LzModule *module = NULL;
    char name[32];
    LzStatus status = LZ_OK;
    for (int i = 0; i < LZ_MODULES_MAX + 2 && status == LZ_OK; i++) {
        (void)snprintf(name, sizeof name, "m%d", i);
        status = lz_module_load(&env.vm, NULL, name, &module);
        if (status == LZ_OK) {
            LZ_CHECK(env.vm.module_count == (size_t)i + 2);
        }
    }
    LZ_CHECK(status != LZ_OK);
    LZ_CHECK(strstr(env.vm.error, "too many modules (limit is 1024)") != NULL);
    LZ_CHECK(env.vm.module_count == LZ_MODULES_MAX);
    finish(&env);
}

static void test_classes_and_extend(void) {
    Src sources[] = {
        {"a",
         "class Box {\n    pub n: int\n}\n"
         "extend string {\n    pub fn tag(self) => \"a:\" + self\n}\n"
         "extend int {\n    pub fn twice(self) => self * 2\n    pub get plus(self) => self + 1\n}\n"
         "fn tagged(s) => s.tag()\n"
         "fn tagged_any(s: any) => s.tag()\n"
         "fn twice_of(n) => n.twice()\n"
         "fn via_map(xs) => xs.map(fn(s) => s.tag())\n"
         "fn plus_of(n) => n.plus\n"
         "fn make() => Box(5)\n"
         "extend Box {\n    pub fn show(self) => \"box \" + string(self.n)\n}\n"
         "fn show_box(b) => b.show()\n"},
        {"b",
         "extend string {\n    pub fn tag(self) => \"b:\" + self\n}\n"
         "fn tagged(s) => s.tag()\n"
         "class Box {\n    pub n: int\n}\n"
         "fn make() => Box(6)\n"},
    };
    Env env;
    setup(&env, sources, 2, true);
    LzModule *a = load(&env, "a");
    LzModule *b = load(&env, "b");
    LZ_CHECK(a != NULL && b != NULL);
    export_global(&env, a, "tagged", "a_tagged");
    export_global(&env, a, "tagged_any", "a_tagged_any");
    export_global(&env, a, "twice_of", "a_twice_of");
    export_global(&env, a, "via_map", "a_via_map");
    export_global(&env, a, "plus_of", "a_plus_of");
    export_global(&env, a, "make", "a_make");
    export_global(&env, a, "show_box", "a_show_box");
    export_global(&env, b, "tagged", "b_tagged");
    export_global(&env, b, "make", "b_make");
    /* Two modules add the same name to string with no clash, and each sees only its own. */
    LZ_CHECK(run_main(&env,
                      "println(a_tagged(\"x\"), b_tagged(\"x\"), a_tagged_any(\"y\"))\n"
                      "println(a_twice_of(4), a_plus_of(4))\n"
                      "println(a_via_map([\"p\", \"q\"]))\n"
                      "let bx = a_make()\nlet other = b_make()\n"
                      "println(typeof(bx), typeof(other), a_show_box(bx))\n") == LZ_OK);
    expect_out(&env, "a:x b:x a:y\n8 5\n[\"a:p\", \"a:q\"]\nBox Box box 5\n");
    /* The classes are different although they share a name, and each knows its module. */
    const LzValue *bx = lz_table_get(&env.vm.globals, lz_vm_intern(&env.vm, "bx", 2));
    const LzValue *other = lz_table_get(&env.vm.globals, lz_vm_intern(&env.vm, "other", 5));
    LZ_CHECK(bx != NULL && other != NULL);
    const LzClass *cb = ((const LzInstance *)bx->as.object)->cls;
    const LzClass *co = ((const LzInstance *)other->as.object)->cls;
    LZ_CHECK(cb != co);
    LZ_CHECK(strcmp(lz_class_module_name(cb), "a.lz") == 0);
    LZ_CHECK(strcmp(lz_class_module_name(co), "b.lz") == 0);
    finish(&env);
}

static void test_extend_invisible_across_modules(void) {
    Src sources[] = {{"a", "extend string {\n    pub fn tag(self) => \"a:\" + self\n}\nfn hello() => \"h\"\n"}};
    Env env;
    setup(&env, sources, 1, true);
    LzModule *a = load(&env, "a");
    export_global(&env, a, "hello", "hello");
    /* a's extend is not seen from main, by a direct call or through an any variable. */
    LZ_CHECK(run_main(&env, "println(hello().tag())\n") != LZ_OK);
    LZ_CHECK(strstr(env.vm.error, "tag") != NULL);
    lz_chunk_free(&env.vm.heap, &env.chunk);
    env.has_chunk = false;
    env.out.length = 0;
    LZ_CHECK(run_main(&env, "let v: any = hello()\nprintln(v.tag())\n") != LZ_OK);
    lz_chunk_free(&env.vm.heap, &env.chunk);
    /* And an extend in main is not seen from a. */
    LZ_CHECK(run_main(&env, "extend string {\n    pub fn tag(self) => \"main:\" + self\n}\nprintln(\"k\".tag())\n") == LZ_OK);
    expect_out(&env, "main:k\n");
    finish(&env);
}

static void test_single_file_extend(void) {
    Env env;
    setup(&env, NULL, 0, false);
    LZ_CHECK(run_main(&env,
                      "extend string {\n    pub fn shout(self) => self.to_upper() + \"!\"\n}\n"
                      "println(\"hi\".shout())\nprintln([\"a\"].map(fn(s) => s.shout()))\n") == LZ_OK);
    expect_out(&env, "HI!\n[\"A!\"]\n");
    finish(&env);
}

/* The same text, compiled as the main program and as an imported module. */
static const char *const IS_MAIN_TEXT =
    "println(\"top\", is_main)\n"
    "fn probe() => is_main\n"
    "println(\"fn\", probe())\n"
    "println(typeof(is_main))\n"
    "let flag: bool = is_main\n"
    "println(\"and\", is_main and 1 < 2, \"or\", is_main or false, \"not\", not is_main)\n"
    "println(if is_main { \"yes\" } else { \"no\" })\n"
    "let doubled = [1, 2].map(fn(n) => if is_main { n * 2 } else { n })\n"
    "println(doubled)\n";

static void test_is_main_in_main(void) {
    Env env;
    setup(&env, NULL, 0, false);
    LZ_CHECK(run_main(&env, IS_MAIN_TEXT) == LZ_OK);
    expect_out(&env, "top true\nfn true\nbool\nand true or true not false\nyes\n[2, 4]\n");
    finish(&env);
}

static void test_is_main_module_value(void) {
    Src sources[] = {{"same", IS_MAIN_TEXT}, {"lib", "pub fn where_am_i() => if is_main { \"main\" } else { \"module\" }\n"}};
    Env env;
    setup(&env, sources, 2, true);
    LzModule *same = load(&env, "same");
    LZ_CHECK(same != NULL);
    expect_out(&env, "top false\nfn false\nbool\nand false or false not true\nno\n[1, 2]\n");
    env.out.length = 0;
    if (env.out.data != NULL) {
        env.out.data[0] = '\0';
    }
    LZ_CHECK(run_main(&env, "import lib\nprintln(is_main, lib.where_am_i())\n") == LZ_OK);
    expect_out(&env, "true module\n");
    finish(&env);
}

static void test_is_main_checks(void) {
    Env env;
    setup(&env, NULL, 0, false);
    LzSource source;
    LzDiagnostics diagnostics;
    const char *bad[] = {"is_main = true\n", "is_main += 1\n", "fn f() {\n    is_main = false\n}\n",
                         "let s: string = is_main\n"};
    const char *message[] = {"cannot assign to 'is_main': it is a builtin constant",
                             "cannot assign to 'is_main': it is a builtin constant",
                             "cannot assign to 'is_main': it is a builtin constant", "type mismatch"};
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        LzChunk chunk;
        lz_source_from_string(&source, "bad.lz", bad[i]);
        lz_chunk_init(&chunk);
        LZ_CHECK(lz_compile(&env.vm, &source, &chunk, &diagnostics) == LZ_ERR_COMPILE);
        LZ_CHECK(diagnostics.count >= 1);
        LZ_CHECK(strstr(diagnostics.items[0].message, message[i]) != NULL);
        lz_chunk_free(&env.vm.heap, &chunk);
    }
    /* A let hides it, like it hides any builtin. */
    LZ_CHECK(run_main(&env, "let is_main = 5\nprintln(is_main)\n") == LZ_OK);
    expect_out(&env, "5\n");
    finish(&env);
}

/* A module that assigns to it does not compile, and says so at its own file. */
static void test_is_main_assignment_in_module(void) {
    Src sources[] = {{"bad", "is_main = true\n"}};
    Env env;
    setup(&env, sources, 1, true);
    LzModule *module = NULL;
    LZ_CHECK(lz_module_load(&env.vm, NULL, "bad", &module) != LZ_OK);
    const LzModuleFailure *failure = lz_vm_module_failure(&env.vm);
    LZ_CHECK(failure != NULL);
    bool found = false;
    for (; failure != NULL; failure = failure->next) {
        for (size_t i = 0; i < failure->diagnostics.count; i++) {
            found = found || strstr(failure->diagnostics.items[i].message, "builtin constant") != NULL;
        }
    }
    LZ_CHECK(found);
    finish(&env);
}

static void test_alloc_failures(void) {
    Src sources[] = {{"a", "let x = 1\nfn f() => x\n"}};
    for (size_t n = 1; n <= 6; n++) {
        Env env;
        setup(&env, sources, 1, true);
        LzModule *module = NULL;
        lz_module_fail_after(n);
        LzStatus status = lz_module_load(&env.vm, NULL, "a", &module);
        lz_module_fail_after(0);
        if (status != LZ_OK) {
            LZ_CHECK(env.vm.module_count == 1);
            LZ_CHECK(module == NULL);
        }
        finish(&env);
    }
}


/* The inline caches of global access, see LzGlobalSite. Each case must read the same on every engine. */

static void test_cache_global_updated_in_loop(void) {
    Env env;
    setup(&env, NULL, 0, false);
    LZ_CHECK(run_main(&env,
                      "let total = 0\nlet step = 1\nfn bump() {\n    step = step + 1\n    return step\n}\n"
                      "for i in 0..5 {\n    total = total + bump()\n    total = total + step\n}\n"
                      "println(total, step)\n") == LZ_OK);
    expect_out(&env, "40 6\n");
    finish(&env);
}

static void test_cache_global_shadows_builtin(void) {
    Env env;
    setup(&env, NULL, 0, false);
    /* len is a builtin at first and a global once defined, in code that already ran it */
    LZ_CHECK(run_main(&env,
                      "fn probe(a) => len(a)\nprintln(probe([1, 2, 3]))\nprintln(probe([1, 2, 3]))\n"
                      "let len = fn(a) => 99\nprintln(probe([1, 2, 3]))\nprintln(len([1]))\n") == LZ_OK);
    expect_out(&env, "3\n3\n99\n99\n");
    finish(&env);
}

static void test_cache_per_module(void) {
    Src sources[] = {{"a", "let x = 1\nfn getx() => x\nfn setx(v) {\n    x = v\n}\n"},
                     {"b", "let x = 2\nfn getx() => x\nfn setx(v) {\n    x = v\n}\n"}};
    Env env;
    setup(&env, sources, 2, true);
    LzModule *a = load(&env, "a");
    LzModule *b = load(&env, "b");
    export_global(&env, a, "getx", "ga");
    export_global(&env, a, "setx", "sa");
    export_global(&env, b, "getx", "gb");
    export_global(&env, b, "setx", "sb");
    LZ_CHECK(run_main(&env,
                      "let x = 0\nlet out = []\nfor i in 0..3 {\n    sa(10 + i)\n    sb(20 + i)\n    x = x + 1\n"
                      "    out.push(ga() + gb() + x)\n}\nprintln(out)\n") == LZ_OK);
    expect_out(&env, "[31, 34, 37]\n");
    finish(&env);
}

static void test_cache_survives_rehash_and_embedder_writes(void) {
    Env env;
    setup(&env, NULL, 0, false);
    LzString *g = lz_vm_intern(&env.vm, "g", 1);
    LZ_CHECK(lz_vm_define_global(&env.vm, g, lz_value_int(1)));
    lz_source_from_string(&env.main_source, "main.lz", "let t = 0\nfor i in 0..3 {\n    t = t + g + len([1])\n}\nprintln(t)\n");
    env.vm.trace_source = &env.main_source;
    lz_chunk_init(&env.chunk);
    env.has_chunk = true;
    LZ_CHECK(lz_compile(&env.vm, &env.main_source, &env.chunk, &env.diagnostics) == LZ_OK);
    LZ_CHECK(lz_vm_run(&env.vm, &env.chunk) == LZ_OK);
    for (int round = 0; round < 3; round++) {
        /* new names grow the table, then the value of g changes in place */
        for (int i = 0; i < 40; i++) {
            char name[24];
            (void)snprintf(name, sizeof name, "pad_%d_%d", round, i);
            LzString *key = lz_vm_intern(&env.vm, name, strlen(name));
            LZ_CHECK(lz_vm_define_global(&env.vm, key, lz_value_int(i)));
        }
        LZ_CHECK(lz_vm_define_global(&env.vm, g, lz_value_int(10 * (round + 1))));
        LZ_CHECK(lz_vm_run(&env.vm, &env.chunk) == LZ_OK);
    }
    expect_out(&env, "6\n33\n63\n93\n");
    finish(&env);
}

static void test_cache_builtin_shadowed_by_embedder(void) {
    Env env;
    setup(&env, NULL, 0, false);
    lz_source_from_string(&env.main_source, "main.lz", "println(len([1, 2]))\n");
    env.vm.trace_source = &env.main_source;
    lz_chunk_init(&env.chunk);
    env.has_chunk = true;
    LZ_CHECK(lz_compile(&env.vm, &env.main_source, &env.chunk, &env.diagnostics) == LZ_OK);
    LZ_CHECK(lz_vm_run(&env.vm, &env.chunk) == LZ_OK);
    LzString *len = lz_vm_intern(&env.vm, "len", 3);
    LzValue seven = lz_value_int(7);
    LZ_CHECK(lz_vm_define_global(&env.vm, len, seven));
    LzStatus status = lz_vm_run(&env.vm, &env.chunk);
    LZ_CHECK(status != LZ_OK); /* 7 is not callable, and a stale builtin would have answered 2 */
    expect_out(&env, "2\n");
    env.vm.stack_count = 0;
    finish(&env);
}

int main(void) {
    test_separate_globals();
    test_function_uses_its_module();
    test_loaded_once();
    test_circular();
    test_nested_load_and_trace();
    test_failures();
    test_limits();
    test_module_limit();
    test_classes_and_extend();
    test_extend_invisible_across_modules();
    test_single_file_extend();
    test_is_main_in_main();
    test_is_main_module_value();
    test_is_main_checks();
    test_is_main_assignment_in_module();
    test_alloc_failures();
    test_cache_global_updated_in_loop();
    test_cache_global_shadows_builtin();
    test_cache_per_module();
    test_cache_survives_rehash_and_embedder_writes();
    test_cache_builtin_shadowed_by_embedder();
    return LZ_TEST_RESULT();
}
