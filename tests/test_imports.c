#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lizard/builtin.h"
#include "lizard/compiler.h"
#include "lizard/link.h"
#include "lizard/module.h"
#include "test_util.h"

typedef struct Src {
    const char *name;
    const char *text;
} Src;

typedef struct Env {
    LzVM vm;
    LzBuffer out;
    LzBuffer err;
    Src *sources;
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

static void setup(Env *env, Src *sources, size_t count) {
    memset(env, 0, sizeof *env);
    lz_vm_init(&env->vm, lz_writer_from_buffer(&env->out), lz_writer_from_buffer(&env->err));
    LZ_CHECK(lz_builtin_install(&env->vm) == LZ_OK);
    env->sources = sources;
    env->source_count = count;
    LzModuleResolver resolver = {env, resolve, NULL};
    lz_vm_set_resolver(&env->vm, &resolver);
}

static LzStatus compile_main(Env *env, const char *text) {
    lz_source_from_string(&env->main_source, "main.lz", text);
    lz_chunk_init(&env->chunk);
    env->has_chunk = true;
    return lz_compile(&env->vm, &env->main_source, &env->chunk, &env->diagnostics);
}

static bool first_error_has(const Env *env, const char *text) {
    return env->diagnostics.count > 0 && strstr(env->diagnostics.items[0].message, text) != NULL;
}

static void finish(Env *env) {
    if (env->has_chunk) {
        lz_chunk_free(&env->vm.heap, &env->chunk);
    }
    lz_vm_release_roots(&env->vm);
    LZ_CHECK(env->vm.heap.live_objects == 0);
    LZ_CHECK(env->vm.heap.live_bytes == 0);
    LZ_CHECK(lz_link_check(&env->vm));
    lz_vm_free(&env->vm);
    lz_buffer_free(&env->out);
    lz_buffer_free(&env->err);
}

static void test_all_forms_compile_without_running(void) {
    Src sources[] = {{"util", "pub fn a() => 1\npub fn b() => 2\nprintln(\"util ran\")\n"},
                     {"tools.format", "println(\"format ran\")\n"}};
    Env env;
    setup(&env, sources, 2);
    LZ_CHECK(compile_main(&env, "import util\nimport util as u\nimport tools.format\nimport tools.format as fmt\n"
                                "from util import a, b as c\n") == LZ_OK);
    finish(&env);

    setup(&env, sources, 2);
    LzStatus status = compile_main(&env, "# comment first\nimport util\nimport tools.format as fmt\nfrom util import a, b as c\n");
    if (status != LZ_OK) {
        fprintf(stderr, "compile: %s\n", env.diagnostics.items[0].message);
    }
    LZ_CHECK(status == LZ_OK);
    LZ_CHECK(env.vm.module_count == 3); /* main, util and tools.format */
    LZ_CHECK(env.out.data == NULL || env.out.data[0] == '\0'); /* nothing ran */
    finish(&env);
}

static void test_prepare_then_run_once(void) {
    Src sources[] = {{"util", "println(\"util ran\")\n"}};
    Env env;
    setup(&env, sources, 1);
    LzModule *module = NULL;
    LZ_CHECK(lz_module_prepare(&env.vm, NULL, "util", &module) == LZ_OK);
    LZ_CHECK(module != NULL && module->state == LZ_MODULE_COMPILED);
    LZ_CHECK(env.out.data == NULL || env.out.data[0] == '\0');
    LzModule *again = NULL;
    LZ_CHECK(lz_module_prepare(&env.vm, NULL, "util", &again) == LZ_OK && again == module);
    LZ_CHECK(lz_module_run(&env.vm, module) == LZ_OK);
    LZ_CHECK(module->state == LZ_MODULE_READY);
    LZ_CHECK(lz_module_run(&env.vm, module) == LZ_OK);
    LzModule *loaded = NULL;
    LZ_CHECK(lz_module_load(&env.vm, NULL, "util", &loaded) == LZ_OK && loaded == module);
    LZ_CHECK(strcmp(env.out.data, "util ran\n") == 0);
    finish(&env);
}

static void test_cycles(void) {
    Src sources[] = {{"a", "import b\n"}, {"b", "import a\n"}, {"c", "import c\n"}, {"d", "import main\n"}, {"main", "let x = 1\n"}};
    Env env;
    setup(&env, sources, 5);
    LzModule *module = NULL;
    LZ_CHECK(lz_module_prepare(&env.vm, NULL, "a", &module) == LZ_ERR_COMPILE);
    const LzModuleFailure *failure = lz_vm_module_failure(&env.vm);
    LZ_CHECK(failure != NULL && failure->next != NULL && failure->next->next == NULL);
    LZ_CHECK(strcmp(failure->next->diagnostics.items[0].message, "circular import of 'a'") == 0);
    LZ_CHECK(strstr(failure->next->diagnostics.items[0].extras.note, "a.lz -> b.lz -> a.lz") != NULL);
    LZ_CHECK(failure->cycle && failure->next->cycle); /* one error for the cycle, the module above adds none */
    LZ_CHECK(failure->diagnostics.count == 0);
    LZ_CHECK(env.vm.module_count == 1); /* nothing half compiled stays */
    LZ_CHECK(lz_module_prepare(&env.vm, NULL, "c", &module) == LZ_ERR_COMPILE);
    LZ_CHECK(strstr(lz_vm_module_failure(&env.vm)->diagnostics.items[0].extras.note, "c.lz -> c.lz") != NULL);
    finish(&env);

    /* An import that leads back to main is a cycle too. */
    setup(&env, sources, 5);
    LZ_CHECK(lz_module_set_main(&env.vm, "main.lz", "main.lz"));
    LZ_CHECK(compile_main(&env, "import d\n") == LZ_ERR_COMPILE);
        const LzModuleFailure *inner = lz_vm_module_failure(&env.vm);
    LZ_CHECK(inner != NULL && strstr(inner->diagnostics.items[0].extras.note, "main.lz -> d.lz -> main.lz") != NULL);
    LZ_CHECK(env.diagnostics.count == 0); /* main adds no second error, the cycle was said in d */
    finish(&env);
}

static void test_failures(void) {
    Src sources[] = {{"bad", "let x = \n"}, {"mid", "import bad\n"}};
    Env env;
    setup(&env, sources, 2);
    LZ_CHECK(compile_main(&env, "import missing\n") == LZ_ERR_COMPILE);
    LZ_CHECK(first_error_has(&env, "cannot find module 'missing'"));
    LZ_CHECK(env.diagnostics.items[0].offset == 7 && env.diagnostics.items[0].length == 7);
    finish(&env);

    setup(&env, sources, 2);
    LZ_CHECK(compile_main(&env, "import mid\n") == LZ_ERR_COMPILE);
    LZ_CHECK(first_error_has(&env, "module 'mid' has errors"));
    const LzModuleFailure *failure = lz_vm_module_failure(&env.vm);
    LZ_CHECK(failure != NULL && failure->next != NULL && failure->next->next == NULL);
    LZ_CHECK(strcmp(failure->source.name, "mid.lz") == 0 && strcmp(failure->next->source.name, "bad.lz") == 0);
    LZ_CHECK(env.vm.module_count == 1); /* neither failed module is kept */
    finish(&env);

    LzVM bare;
    LzBuffer out = {0};
    LzBuffer err = {0};
    lz_vm_init(&bare, lz_writer_from_buffer(&out), lz_writer_from_buffer(&err));
    LzChunk chunk;
    LzDiagnostics diagnostics;
    LzSource source;
    lz_source_from_string(&source, "main.lz", "import util\n");
    lz_chunk_init(&chunk);
    LZ_CHECK(lz_compile(&bare, &source, &chunk, &diagnostics) == LZ_ERR_COMPILE);
    LZ_CHECK(strstr(diagnostics.items[0].message, "modules are not available here") != NULL);
    lz_chunk_free(&bare.heap, &chunk);
    lz_vm_free(&bare);
    lz_buffer_free(&out);
    lz_buffer_free(&err);
}

typedef struct Bad {
    const char *text;
    const char *message;
} Bad;

static void test_syntax_errors(void) {
    Src sources[] = {{"util", "pub fn a() => 1\n"}, {"other", "pub fn a() => 2\n"}};
    Bad cases[] = {
        {"print(1)\nimport util\n", "`import` can only be used at the top of a file"},
        {"fn f() {\n    import util\n}\n", "`import` can only be used at the top of a file"},
        {"if true {\n    import util\n}\n", "`import` can only be used at the top of a file"},
        {"print(1)\nfrom util import a\n", "`from ... import` can only be used at the top of a file"},
        {"pub import util\n", "`pub` cannot go in front of `import`"},
        {"import\n", "expected a module name after 'import'"},
        {"import util.\n", "expected a name after '.' in the module name"},
        {"from util import\n", "expected a name after 'import'"},
        {"from import a\n", "expected a module name after 'from'"},
        {"from util\n", "expected 'import' after the module name"},
        {"from util import a,\n", "expected a name after 'import'"},
        {"import util as\n", "expected a name after 'as'"},
        {"import util\nimport other as util\n", "`util` is imported twice"},
        {"import util\nfn util() {}\n", "`util` is already declared in this file"},
        {"from util import a\nlet a = 1\n", "`a` is already declared in this file"},
        {"from util import a\nfrom other import a\n", "`a` is imported twice"},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        Env env;
        setup(&env, sources, 2);
        LzStatus status = compile_main(&env, cases[i].text);
        if (status != LZ_ERR_COMPILE || !first_error_has(&env, cases[i].message)) {
            fprintf(stderr, "case %zu [%s]: %s\n", i, cases[i].text,
                    env.diagnostics.count > 0 ? env.diagnostics.items[0].message : "compiled");
        }
        LZ_CHECK(status == LZ_ERR_COMPILE && first_error_has(&env, cases[i].message));
        finish(&env);
    }
}

static void test_contextual_words(void) {
    Env env;
    setup(&env, NULL, 0);
    LZ_CHECK(compile_main(&env, "let import = 1\nlet from = 2\nlet as = 3\nprintln(import + from + as)\n"
                                "fn f(import) => import\nprintln(f(from))\n") == LZ_OK);
    LZ_CHECK(lz_vm_run(&env.vm, &env.chunk) == LZ_OK);
    LZ_CHECK(strcmp(env.out.data, "6\n2\n") == 0);
    finish(&env);
}

static void test_import_depth(void) {
    enum { COUNT = LZ_IMPORT_DEPTH_MAX + 20 };
    static char names[COUNT][16];
    static char texts[COUNT][32];
    static Src sources[COUNT];
    for (int i = 0; i < COUNT; i++) {
        (void)snprintf(names[i], sizeof names[i], "m%d", i);
        (void)snprintf(texts[i], sizeof texts[i], i + 1 < COUNT ? "import m%d\n" : "let x = 1\n", i + 1);
        sources[i].name = names[i];
        sources[i].text = texts[i];
    }
    Env env;
    setup(&env, sources, COUNT);
    LZ_CHECK(compile_main(&env, "import m0\n") == LZ_ERR_COMPILE);
    LZ_CHECK(env.vm.module_count == 1);
    finish(&env);
}

static void test_alloc_failures(void) {
    Src sources[] = {{"util", "pub fn a() => 1\n"}, {"mid", "import util\nprintln(1)\n"}};
    for (size_t n = 1; n < 40; n++) {
        Env env;
        setup(&env, sources, 2);
        lz_module_fail_after(n);
        LzStatus status = compile_main(&env, "import mid\nfrom util import a\n");
        lz_module_fail_after(0);
        if (status == LZ_OK) {
            finish(&env);
            break;
        }
        LZ_CHECK(env.vm.module_count <= 3);
        finish(&env);
    }
}

static void test_run_imports(void) {
    Src sources[] = {{"util", "println(\"util\")\npub fn add(a: int, b: int) -> int => a + b\npub const LIMIT = 10\n"},
                     {"mid", "import util\nprintln(\"mid\")\npub fn twice(n: int) => util.add(n, n)\n"},
                     {"boom", "println(\"boom\")\nlet zero = 0\nprintln(1 / zero)\n"}};
    Env env;
    setup(&env, sources, 3);
    LZ_CHECK(compile_main(&env, "import util\nimport mid\nfrom util import add as plus, LIMIT\n"
                                "println(util.add(1, 2))\nprintln(mid.twice(4))\nprintln(plus(5, 6) + LIMIT)\n") == LZ_OK);
    LZ_CHECK(lz_vm_run(&env.vm, &env.chunk) == LZ_OK);
    /* Each module runs once, in the order of the imports. */
    LZ_CHECK(strcmp(env.out.data, "util\nmid\n3\n8\n21\n") == 0);
    finish(&env);

    setup(&env, sources, 3);
    LZ_CHECK(compile_main(&env, "println(\"start\")\nimport boom\n") == LZ_ERR_COMPILE); /* imports come first */
    finish(&env);

    setup(&env, sources, 3);
    LZ_CHECK(compile_main(&env, "import boom\nprintln(\"unreached\")\n") == LZ_OK);
    LZ_CHECK(lz_vm_run(&env.vm, &env.chunk) != LZ_OK);
    LZ_CHECK(strcmp(env.out.data, "boom\n") == 0);
    LZ_CHECK(env.vm.error[0] != '\0');
    finish(&env);
}

typedef struct TypeCase {
    const char *text;
    const char *output; /* expected output when it compiles and runs, else NULL */
    const char *error;  /* text in the first error, else NULL */
} TypeCase;

static void test_cross_module_types(void) {
    static const char shapes[] =
        "pub class Point {\n    pub x: int\n    pub y: int = 0\n    pub read z: int = 5\n    secret: int = 7\n"
        "    pub fn sum(self) => self.x + self.y\n}\n"
        "pub trait Shape {\n    fn area(self)\n}\n"
        "pub class Sq : Shape {\n    pub s: int\n    pub fn area(self) => self.s * self.s\n}\n"
        "pub error MyError\npub fn make() => Point(1, 2)\n";
    static const TypeCase cases[] = {
        {"import util\nlet p: util.Point = util.Point(1, 2)\nprintln(p.sum())\n", "3\n", NULL},
        {"from util import Point\nlet p: Point = Point(1, 2)\nprintln(p.sum())\n", "3\n", NULL},
        {"import util\nlet p = util.make()\nprintln(p is util.Point)\nprintln(p is not util.Sq)\n", "true\ntrue\n", NULL},
        {"import util\nfn f(x: util.Shape) => x.area()\nprintln(f(util.Sq(3)))\n", "9\n", NULL},
        {"import util\nlet q: util.Point? = nil\nlet a: util.Point[] = [util.make()]\nprintln(q)\nprintln(a.len())\n",
         "nil\n1\n", NULL},
        {"import util\nclass B : util.Shape {\n    pub fn area(self) => 4\n}\nprintln(B().area())\n", "4\n", NULL},
        {"import util\nclass C : util.Point {\n}\nprintln(C(1, 2).sum())\n", "3\n", NULL},
        {"import util\ntry {\n    throw util.MyError(\"boom\")\n} catch util.MyError as e {\n    println(\"caught\")\n}\n",
         "caught\n", NULL},
        {"import util\nextend util.Point {\n    pub fn double(self) => self.x * 2\n}\nprintln(util.make().double())\n", "2\n", NULL},
        {"import util\nlet p: util.Point = 3\n", NULL, "type mismatch: expected Point"},
        {"import util\nfn f(p: util.Point) => p.x\nf(5)\n", NULL, "argument 'p' of f() expects Point"},
        {"import util\nlet p: util.Nope = 1\n", NULL, "has no name 'Nope'"},
        {"import util\nlet p: util.make = 1\n", NULL, "'make' is not a type"},
        {"import util\nlet p: util = 1\n", NULL, "'util' is a module, not a type"},
        {"import util\nclass D : util.make {\n}\n", NULL, "is not a class or a trait"},
        {"import util\ntry {\n} catch util.Point as e {\n}\n", NULL, "'Point' is not an error type"},
    };
    static const TypeCase runtime[] = {
        {"import util\nlet p = util.Point(1, 2)\nlet q: util.Sq = p\n", NULL, "expected Sq, got Point"},
        {"import util\nlet p = util.make()\np.z = 3\n", NULL, "'z' is read-only outside Point"},
        {"import util\nlet p = util.make()\nprintln(p.secret)\n", NULL, "'secret' is private to Point"},
        {"import util\nlet p = util.make()\nprintln(p.nothing)\n", NULL, "Point has no field 'nothing'"},
        {"import util\nlet p = util.Point(\"a\", 2)\n", NULL, "Point.x expects int, got string"},
        {"import util\nlet p = util.Point(1, 2, 3, 4)\n", NULL, "takes at most 3 arguments"},
    };
    Src sources[] = {{"util", shapes}};
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        Env env;
        setup(&env, sources, 1);
        LzStatus status = compile_main(&env, cases[i].text);
        if (cases[i].error != NULL) {
            if (status != LZ_ERR_COMPILE) {
                (void)fprintf(stderr, "case %zu: expected a compile error, got status %d\n", i, (int)status);
            }
            LZ_CHECK(status == LZ_ERR_COMPILE);
            if (status == LZ_ERR_COMPILE && !first_error_has(&env, cases[i].error)) {
                (void)fprintf(stderr, "case %zu: first error is '%s'\n", i, env.diagnostics.items[0].message);
                LZ_CHECK(false);
            }
        } else {
            LZ_CHECK(status == LZ_OK);
            if (status == LZ_OK) {
                LZ_CHECK(lz_vm_run(&env.vm, &env.chunk) == LZ_OK);
                if (strcmp(env.out.data == NULL ? "" : env.out.data, cases[i].output) != 0) {
                    (void)fprintf(stderr, "case %zu: output '%s'\n", i, env.out.data == NULL ? "" : env.out.data);
                    LZ_CHECK(false);
                }
            } else {
                (void)fprintf(stderr, "case %zu: compile failed: %s\n", i, env.diagnostics.items[0].message);
            }
        }
        finish(&env);
    }
    for (size_t i = 0; i < sizeof runtime / sizeof runtime[0]; i++) {
        Env env;
        setup(&env, sources, 1);
        LZ_CHECK(compile_main(&env, runtime[i].text) == LZ_OK);
        LZ_CHECK(lz_vm_run(&env.vm, &env.chunk) != LZ_OK);
        if (strstr(env.vm.error, runtime[i].error) == NULL) {
            (void)fprintf(stderr, "runtime %zu: error is '%s'\n", i, env.vm.error);
            LZ_CHECK(false);
        }
        finish(&env);
    }
}

static void test_pub_names_cannot_be_assigned(void) {
    static const TypeCase cases[] = {
        {"pub fn foo() => 1\nfoo = 1\n", NULL, "cannot assign to 'foo': it is pub, so other modules rely on it staying the same"},
        {"pub fn foo() => 1\nfoo += 1\n", NULL, "it is pub"},
        {"pub fn foo() => 1\nfn g() {\n    foo = 2\n}\n", NULL, "it is pub"},
        {"pub class K {\n}\nK = 1\n", NULL, "cannot assign to 'K': it is pub"},
        {"pub trait T {\n    fn a(self)\n}\nT = 1\n", NULL, "cannot assign to 'T': it is pub"},
        {"pub error E\nE = 1\n", NULL, "cannot assign to 'E': it is pub"},
        {"pub type Id = int\nId = 1\n", NULL, "cannot assign to 'Id': it is pub"},
        {"pub extend int {\n}\n", NULL, "'pub' cannot be used on 'extend'"},
        /* Not pub: the old messages stay. */
        {"fn foo() => 1\nfoo = 1\n", NULL, "cannot assign to constant 'foo'"},
        {"class K {\n}\nK = 1\n", NULL, "cannot assign to constant 'K'"},
        {"pub const C = 1\nC = 2\n", NULL, "cannot assign to constant 'C'"},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        Env env;
        setup(&env, NULL, 0);
        LZ_CHECK(compile_main(&env, cases[i].text) == LZ_ERR_COMPILE);
        if (env.diagnostics.count == 0 || !first_error_has(&env, cases[i].error)) {
            (void)fprintf(stderr, "pub case %zu: first error is '%s'\n", i,
                          env.diagnostics.count > 0 ? env.diagnostics.items[0].message : "(none)");
            LZ_CHECK(false);
        }
        finish(&env);
    }
}

static void test_ambiguous_type_names(void) {
    Src sources[] = {{"pt", "pub class Point {\n    pub x: int\n}\n"},
                     {"geo", "pub class Point {\n    pub x: int\n    pub y: int = 0\n}\n"}};
    Env env;
    setup(&env, sources, 2);
    LZ_CHECK(compile_main(&env, "import pt\nimport geo\nlet a: pt.Point = pt.Point(1)\nlet b: geo.Point = a\n") == LZ_ERR_COMPILE);
    LZ_CHECK(first_error_has(&env, "expected geo.Point, got pt.Point"));
    finish(&env);

    setup(&env, sources, 2);
    LZ_CHECK(compile_main(&env, "import pt\nimport geo\nfn f(p: pt.Point) => p.x\nlet g: any = geo.Point(1, 2)\nf(g)\n") == LZ_OK);
    LZ_CHECK(lz_vm_run(&env.vm, &env.chunk) != LZ_OK);
    LZ_CHECK(strstr(env.vm.error, "expected pt.Point, got geo.Point") != NULL);
    finish(&env);

    /* The names stay short when nothing is ambiguous. */
    setup(&env, sources, 2);
    LZ_CHECK(compile_main(&env, "import pt\nimport geo\nlet a: pt.Point = pt.Point(1)\nlet b: pt.Point? = nil\nlet c: int = a\n") == LZ_ERR_COMPILE);
    LZ_CHECK(first_error_has(&env, "expected int, got Point"));
    finish(&env);

    setup(&env, sources, 2);
    LZ_CHECK(compile_main(&env, "import pt\nimport geo\nlet g: any = geo.Point(1, 2)\nprintln(g is pt.Point)\nprintln(g is geo.Point)\nprintln(typeof(g))\n") == LZ_OK);
    LZ_CHECK(lz_vm_run(&env.vm, &env.chunk) == LZ_OK);
    LZ_CHECK(strcmp(env.out.data, "false\ntrue\nPoint\n") == 0);
    finish(&env);
}

static void test_failed_module_reported_once(void) {
    Src sources[] = {{"bad", "let x = \n"}, {"mid", "import bad\n"}};
    Env env;
    setup(&env, sources, 2);
    LZ_CHECK(compile_main(&env, "import bad\nimport mid\n") == LZ_ERR_COMPILE);
    size_t records = 0;
    size_t bad_records = 0;
    for (const LzModuleFailure *f = lz_vm_module_failure(&env.vm); f != NULL; f = f->next) {
        records++;
        if (f->id != NULL && strstr(f->id, "bad") != NULL) {
            bad_records++;
        }
    }
    LZ_CHECK(records == 2); /* bad once, then mid */
    LZ_CHECK(bad_records == 1);
    LZ_CHECK(env.diagnostics.count == 2); /* main: the import of bad and the import of mid */
    finish(&env);
}

static void test_limits_through_import(void) {
    /* Fuel and the heap limit apply to code of an imported module, which runs inside the run of main. */
    Src spin[] = {{"spin", "let n = 0\nwhile true {\n    n += 1\n}\n"}};
    Env env;
    setup(&env, spin, 1);
    LZ_CHECK(compile_main(&env, "import spin\n") == LZ_OK);
    lz_vm_set_fuel(&env.vm, 5000);
    LZ_CHECK(lz_vm_run(&env.vm, &env.chunk) == LZ_ERR_FUEL);
    finish(&env);

    Src fat[] = {{"fat", "let items = []\nlet i = 0\nwhile i < 100000 {\n    items.push([i, i, i, i])\n    i += 1\n}\n"}};
    setup(&env, fat, 1);
    LZ_CHECK(compile_main(&env, "import fat\n") == LZ_OK);
    lz_vm_set_heap_limit(&env.vm, env.vm.heap.live_bytes + 64 * 1024);
    LZ_CHECK(lz_vm_run(&env.vm, &env.chunk) == LZ_ERR_HEAP_LIMIT);
    finish(&env);

    /* Whatever the limit is, a run with imports ends cleanly: ok or an error, never a crash. */
    Src two[] = {{"util", "pub fn a() => [1, 2, 3]\npub class P {\n    pub x: int\n}\n"}};
    for (size_t extra = 0; extra < 4096; extra += 64) {
        setup(&env, two, 1);
        LzStatus status = compile_main(&env, "import util\nfrom util import P\nprintln(util.a().len())\nprintln(P(1).x)\n");
        if (status == LZ_OK) {
            lz_vm_set_heap_limit(&env.vm, env.vm.heap.live_bytes + extra);
            status = lz_vm_run(&env.vm, &env.chunk);
            LZ_CHECK(status == LZ_OK || status == LZ_ERR_HEAP_LIMIT || status == LZ_ERR_MEMORY);
        }
        finish(&env);
    }
}

static Src *chain_sources(size_t count, const char *prefix) {
    Src *sources = calloc(count, sizeof *sources);
    for (size_t i = 0; i < count; i++) {
        char name[32];
        char text[64];
        (void)snprintf(name, sizeof name, "%s%zu", prefix, i);
        if (i + 1 < count) {
            (void)snprintf(text, sizeof text, "import %s%zu\n", prefix, i + 1);
        } else {
            (void)snprintf(text, sizeof text, "let end = 1\n");
        }
        char *name_copy = malloc(strlen(name) + 1);
        char *text_copy = malloc(strlen(text) + 1);
        memcpy(name_copy, name, strlen(name) + 1);
        memcpy(text_copy, text, strlen(text) + 1);
        sources[i].name = name_copy;
        sources[i].text = text_copy;
    }
    return sources;
}

static void chain_free(Src *sources, size_t count) {
    for (size_t i = 0; i < count; i++) {
        free((void *)sources[i].name);
        free((void *)sources[i].text);
    }
    free(sources);
}

static void test_module_count_and_depth_limits(void) {
    /* Depth: a chain just inside the limit loads, one past it says so. */
    size_t inside = LZ_IMPORT_DEPTH_MAX - 2;
    Src *chain = chain_sources(inside, "c");
    Env env;
    setup(&env, chain, inside);
    LZ_CHECK(compile_main(&env, "import c0\n") == LZ_OK);
    finish(&env);
    chain_free(chain, inside);

    size_t beyond = LZ_IMPORT_DEPTH_MAX + 2;
    chain = chain_sources(beyond, "d");
    setup(&env, chain, beyond);
    LZ_CHECK(compile_main(&env, "import d0\n") == LZ_ERR_COMPILE);
    bool said = false;
    for (const LzModuleFailure *f = lz_vm_module_failure(&env.vm); f != NULL; f = f->next) {
        for (size_t i = 0; i < f->diagnostics.count; i++) {
            if (strstr(f->diagnostics.items[i].message, "imports are nested too deeply") != NULL) {
                said = true;
            }
        }
    }
    LZ_CHECK(said);
    LZ_CHECK(env.vm.module_count < LZ_MODULES_MAX);
    finish(&env);
    chain_free(chain, beyond);

    /* Count: a file takes at most 64 imported names, so a tree of 32 groups of 32 leaves gets past the limit. */
    enum { GROUPS = 32, LEAVES = 32, TOTAL = GROUPS + GROUPS * LEAVES };
    Src *tree = calloc(TOTAL, sizeof *tree);
    char main_text[GROUPS * 16 + 1];
    size_t main_used = 0;
    for (size_t g = 0; g < GROUPS; g++) {
        char name[32];
        (void)snprintf(name, sizeof name, "g%zu", g);
        char *group_name = malloc(strlen(name) + 1);
        memcpy(group_name, name, strlen(name) + 1);
        char *group_text = calloc(1, LEAVES * 16 + 1);
        size_t used = 0;
        for (size_t k = 0; k < LEAVES; k++) {
            char leaf[32];
            (void)snprintf(leaf, sizeof leaf, "l%zu_%zu", g, k);
            char *leaf_name = malloc(strlen(leaf) + 1);
            memcpy(leaf_name, leaf, strlen(leaf) + 1);
            tree[GROUPS + g * LEAVES + k].name = leaf_name;
            tree[GROUPS + g * LEAVES + k].text = "let v = 1\n";
            used += (size_t)snprintf(group_text + used, LEAVES * 16 + 1 - used, "import %s\n", leaf);
        }
        tree[g].name = group_name;
        tree[g].text = group_text;
        main_used += (size_t)snprintf(main_text + main_used, sizeof main_text - main_used, "import g%zu\n", g);
    }
    setup(&env, tree, TOTAL);
    LZ_CHECK(compile_main(&env, main_text) == LZ_ERR_COMPILE);
    bool too_many = false;
    for (const LzModuleFailure *f = lz_vm_module_failure(&env.vm); f != NULL; f = f->next) {
        for (size_t d = 0; d < f->diagnostics.count; d++) {
            if (strstr(f->diagnostics.items[d].message, "too many modules") != NULL ||
                strstr(f->diagnostics.items[d].extras.note, "too many modules") != NULL) {
                too_many = true;
            }
        }
    }
    for (size_t d = 0; d < env.diagnostics.count; d++) {
        too_many = too_many || strstr(env.diagnostics.items[d].message, "too many modules") != NULL ||
                   strstr(env.diagnostics.items[d].message, "has errors") != NULL;
    }
    LZ_CHECK(too_many);
    LZ_CHECK(env.vm.module_count <= LZ_MODULES_MAX);
    finish(&env);
    for (size_t g = 0; g < GROUPS; g++) {
        free((void *)tree[g].name);
        free((void *)tree[g].text);
        for (size_t k = 0; k < LEAVES; k++) {
            free((void *)tree[GROUPS + g * LEAVES + k].name);
        }
    }
    free(tree);
}

static void test_import_op_without_loader(void) {
    /* Regression: LZ_OP_IMPORT must fail with an error, not dereference what is not there, when the
       module is not in the list or the VM has no way to run modules. */
    Src sources[] = {{"util", "pub fn a() => 1\n"}};
    Env env;
    setup(&env, sources, 1);
    LZ_CHECK(compile_main(&env, "import util\nprintln(util.a())\n") == LZ_OK);
    LzStatus (*saved)(struct LzVM *, struct LzModule *) = env.vm.module_run;
    env.vm.module_run = NULL;
    LZ_CHECK(lz_vm_run(&env.vm, &env.chunk) != LZ_OK);
    LZ_CHECK(strstr(env.vm.error, "is not loaded") != NULL);
    env.vm.module_run = saved;
    finish(&env);

    setup(&env, sources, 1);
    LZ_CHECK(compile_main(&env, "import util\nprintln(util.a())\n") == LZ_OK);
    LzModule *module = env.vm.modules;
    while (module != NULL && (module->id == NULL || strstr(module->id, "util") == NULL)) {
        module = module->next;
    }
    LZ_CHECK(module != NULL);
    if (module != NULL) {
        LzChunk *held = module->chunk;
        module->chunk = NULL; /* a module with nothing compiled in it */
        module->state = LZ_MODULE_READY;
        LZ_CHECK(lz_vm_run(&env.vm, &env.chunk) != LZ_ERR_MEMORY);
        module->chunk = held;
    }
    finish(&env);
}

int main(void) {
    test_limits_through_import();
    test_module_count_and_depth_limits();
    test_import_op_without_loader();
    test_failed_module_reported_once();
    test_ambiguous_type_names();
    test_pub_names_cannot_be_assigned();
    test_cross_module_types();
    test_run_imports();
    test_all_forms_compile_without_running();
    test_prepare_then_run_once();
    test_cycles();
    test_failures();
    test_syntax_errors();
    test_contextual_words();
    test_import_depth();
    test_alloc_failures();
    return LZ_TEST_RESULT();
}
