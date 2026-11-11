#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "lizard/builtin.h"
#include "lizard/compiler.h"
#include "lizard/interface.h"
#include "test_util.h"

/*
 * The interface of a compiled unit, read only through lizard/interface.h. The printer below lives
 * here and nowhere else: the library has no code that prints an interface.
 */

static void put(LzBuffer *out, const char *text) {
    LzWriter writer = lz_writer_from_buffer(out);
    (void)lz_writer_write_cstr(&writer, text);
}

static void put_sig(LzBuffer *out, const LzIfaceSig *sig) {
    put(out, "(");
    for (size_t i = 0; i < lz_interface_sig_param_count(sig); i++) {
        if (i > 0) {
            put(out, ", ");
        }
        if (lz_interface_sig_param_is_rest(sig, i)) {
            put(out, "*");
        }
        put(out, lz_interface_sig_param_name(sig, i));
        put(out, ": ");
        put(out, lz_interface_sig_param_type(sig, i));
        if (lz_interface_sig_param_has_default(sig, i)) {
            put(out, " =");
        }
    }
    put(out, ") -> ");
    put(out, lz_interface_sig_result(sig));
}

static void put_member(LzBuffer *out, const LzIfaceMember *member) {
    LzIfaceMemberKind kind = lz_interface_member_kind(member);
    put(out, "  ");
    put(out, lz_interface_member_kind_name(kind));
    if (kind == LZ_IFACE_MEMBER_FIELD && lz_interface_member_is_read_only(member)) {
        put(out, " read");
    }
    if (lz_interface_member_is_override(member)) {
        put(out, " override");
    }
    put(out, " ");
    put(out, lz_interface_member_name(member));
    if (lz_interface_member_type(member) != NULL) {
        put(out, ": ");
        put(out, lz_interface_member_type(member));
        if (lz_interface_member_has_default(member)) {
            put(out, " =");
        }
    }
    if (lz_interface_member_sig(member) != NULL) {
        put_sig(out, lz_interface_member_sig(member));
    }
    if (lz_interface_member_is_required(member)) {
        put(out, " required");
    }
    if (lz_interface_member_has_explicit_init(member)) {
        put(out, " explicit");
    }
    put(out, "\n");
}

static void put_interface(LzBuffer *out, const LzInterface *iface) {
    for (size_t i = 0; i < lz_interface_count(iface); i++) {
        const LzIfaceEntry *entry = lz_interface_entry(iface, i);
        put(out, lz_interface_kind_name(lz_interface_entry_kind(entry)));
        put(out, " ");
        put(out, lz_interface_entry_name(entry));
        if (lz_interface_entry_sig(entry) != NULL) {
            put_sig(out, lz_interface_entry_sig(entry));
        }
        if (lz_interface_entry_type(entry) != NULL) {
            put(out, ": ");
            put(out, lz_interface_entry_type(entry));
        }
        if (lz_interface_entry_parent(entry) != NULL) {
            put(out, " : ");
            put(out, lz_interface_entry_parent(entry));
        }
        for (size_t t = 0; t < lz_interface_entry_trait_count(entry); t++) {
            put(out, " + ");
            put(out, lz_interface_entry_trait(entry, t));
        }
        put(out, "\n");
        for (size_t m = 0; m < lz_interface_entry_member_count(entry); m++) {
            put_member(out, lz_interface_entry_member(entry, m));
        }
    }
}

/* Compiles `text` (without running it), prints its interface and compares. */
static void check_interface(const char *text, const char *expected) {
    LzVM vm;
    LzBuffer out = {0};
    LzBuffer err = {0};
    LzBuffer dump = {0};
    LzChunk chunk;
    LzDiagnostics diagnostics;
    LzSource source;
    memset(&vm, 0, sizeof vm);
    lz_vm_init(&vm, lz_writer_from_buffer(&out), lz_writer_from_buffer(&err));
    LZ_CHECK(lz_builtin_install(&vm) == LZ_OK);
    lz_source_from_string(&source, "t.lz", text);
    lz_chunk_init(&chunk);
    LZ_CHECK(lz_compile(&vm, &source, &chunk, &diagnostics) == LZ_OK);
    const LzInterface *iface = lz_interface_of(&chunk);
    LZ_CHECK(iface != NULL);
    put_interface(&dump, iface);
    const char *actual = dump.data != NULL ? dump.data : "";
    if (strcmp(actual, expected) != 0) {
        fprintf(stderr, "program:\n%s\nexpected interface:\n%s\nactual interface:\n%s\n", text, expected, actual);
        lz_test_failures++;
    }
    lz_buffer_free(&dump);
    lz_chunk_free(&vm.heap, &chunk);
    lz_vm_release_roots(&vm);
    lz_vm_free(&vm);
    lz_buffer_free(&out);
    lz_buffer_free(&err);
}

/* The program must not compile: one error, with this message, and the labels asked for. */
static void check_error(const char *text, const char *message, const char *label, const char *note,
                        bool has_fix) {
    LzVM vm;
    LzBuffer out = {0};
    LzBuffer err = {0};
    LzChunk chunk;
    LzDiagnostics diagnostics;
    LzSource source;
    memset(&vm, 0, sizeof vm);
    lz_vm_init(&vm, lz_writer_from_buffer(&out), lz_writer_from_buffer(&err));
    LZ_CHECK(lz_builtin_install(&vm) == LZ_OK);
    lz_source_from_string(&source, "t.lz", text);
    lz_chunk_init(&chunk);
    LzStatus status = lz_compile(&vm, &source, &chunk, &diagnostics);
    bool ok = status == LZ_ERR_COMPILE && diagnostics.count == 1 &&
              strcmp(diagnostics.items[0].message, message) == 0 &&
              strcmp(diagnostics.items[0].extras.label, label) == 0 &&
              strcmp(diagnostics.items[0].extras.note, note) == 0 &&
              diagnostics.items[0].extras.has_fix == has_fix;
    if (!ok) {
        fprintf(stderr, "program:\n%s\nwanted [%s] [%s] [%s] fix=%d; got status %d count %zu [%s] [%s] [%s] fix=%d\n",
                text, message, label, note, (int)has_fix, (int)status, diagnostics.count,
                diagnostics.count > 0 ? diagnostics.items[0].message : "",
                diagnostics.count > 0 ? diagnostics.items[0].extras.label : "",
                diagnostics.count > 0 ? diagnostics.items[0].extras.note : "",
                diagnostics.count > 0 ? (int)diagnostics.items[0].extras.has_fix : 0);
    }
    LZ_CHECK(ok);
    lz_chunk_free(&vm.heap, &chunk);
    lz_vm_release_roots(&vm);
    lz_vm_free(&vm);
    lz_buffer_free(&out);
    lz_buffer_free(&err);
}

/* A program that compiles and runs as it would without `pub`. */
static void check_runs(const char *text, const char *expected) {
    LzVM vm;
    LzBuffer out = {0};
    LzBuffer err = {0};
    LzChunk chunk;
    LzDiagnostics diagnostics;
    LzSource source;
    memset(&vm, 0, sizeof vm);
    lz_vm_init(&vm, lz_writer_from_buffer(&out), lz_writer_from_buffer(&err));
    LZ_CHECK(lz_builtin_install(&vm) == LZ_OK);
    lz_source_from_string(&source, "t.lz", text);
    lz_chunk_init(&chunk);
    LZ_CHECK(lz_compile(&vm, &source, &chunk, &diagnostics) == LZ_OK);
    LZ_CHECK(lz_vm_run(&vm, &chunk) == LZ_OK);
    const char *actual = out.data != NULL ? out.data : "";
    if (strcmp(actual, expected) != 0) {
        fprintf(stderr, "program:\n%s\nexpected [%s] got [%s]\n", text, expected, actual);
        lz_test_failures++;
    }
    lz_chunk_free(&vm.heap, &chunk);
    lz_vm_release_roots(&vm);
    lz_vm_free(&vm);
    lz_buffer_free(&out);
    lz_buffer_free(&err);
}

/* `pub let` is refused: the error, its help (a fix when the variable never changes), no entry,
 * and, with a fix, a result that compiles. */
static void check_pub_let(const char *text, const char *help, const char *fixed_text) {
    LzVM vm;
    LzBuffer out = {0};
    LzBuffer err = {0};
    LzChunk chunk;
    LzDiagnostics diagnostics;
    LzSource source;
    memset(&vm, 0, sizeof vm);
    lz_vm_init(&vm, lz_writer_from_buffer(&out), lz_writer_from_buffer(&err));
    LZ_CHECK(lz_builtin_install(&vm) == LZ_OK);
    lz_source_from_string(&source, "t.lz", text);
    lz_chunk_init(&chunk);
    LzStatus status = lz_compile(&vm, &source, &chunk, &diagnostics);
    bool ok = status == LZ_ERR_COMPILE && diagnostics.count == 1;
    if (ok) {
        const LzDiagnostic *item = &diagnostics.items[0];
        ok = strcmp(item->message, "'pub' cannot be used on 'let'") == 0 &&
             strcmp(item->extras.label, "a variable can change, so other code could not rely on it") == 0 &&
             strcmp(item->extras.note, "`pub` is for things that stay the same, or that you reach through a function") == 0 &&
             item->length == 3 && memcmp(text + item->offset, "let", 3) == 0;
        if (fixed_text != NULL) {
            ok = ok && item->extras.has_fix && strcmp(item->extras.fix_text, "const") == 0 &&
                 item->extras.fix_offset == item->offset && item->extras.fix_delete == 3;
        } else {
            ok = ok && !item->extras.has_fix && strcmp(item->extras.help, help) == 0;
        }
    }
    if (!ok) {
        fprintf(stderr, "program:\n%s\nwrong `pub let` error (status %d, count %zu)\n", text, (int)status,
                diagnostics.count);
    }
    LZ_CHECK(ok);
    const LzInterface *iface = lz_interface_of(&chunk);
    LZ_CHECK(iface == NULL || lz_interface_count(iface) == 0);
    lz_chunk_free(&vm.heap, &chunk);
    lz_vm_release_roots(&vm);
    lz_vm_free(&vm);
    lz_buffer_free(&out);
    lz_buffer_free(&err);
    if (fixed_text != NULL) {
        /* the fix, applied by hand, is the program that compiles */
        char patched[256];
        size_t at = strstr(text, "let") - text;
        (void)snprintf(patched, sizeof patched, "%.*sconst%s", (int)at, text, text + at + 3);
        LZ_CHECK(strcmp(patched, fixed_text) == 0);
        check_interface(fixed_text, strstr(fixed_text, "pub const names") != NULL ? "const names: string[]\n"
                                                                                  : "const limit: int\n");
    }
}


/* The signature of the pub fn `name`, written out through the API: <type params>(params) -> result [declared]. */
static void check_fn_sig(const char *text, const char *name, const char *expected) {
    LzVM vm;
    LzBuffer out = {0};
    LzBuffer err = {0};
    LzBuffer dump = {0};
    LzChunk chunk;
    LzDiagnostics diagnostics;
    LzSource source;
    memset(&vm, 0, sizeof vm);
    lz_vm_init(&vm, lz_writer_from_buffer(&out), lz_writer_from_buffer(&err));
    LZ_CHECK(lz_builtin_install(&vm) == LZ_OK);
    lz_source_from_string(&source, "t.lz", text);
    lz_chunk_init(&chunk);
    LZ_CHECK(lz_compile(&vm, &source, &chunk, &diagnostics) == LZ_OK);
    const LzInterface *iface = lz_interface_of(&chunk);
    LZ_CHECK(iface != NULL);
    const LzIfaceSig *sig = NULL;
    for (size_t i = 0; iface != NULL && i < lz_interface_count(iface); i++) {
        const LzIfaceEntry *entry = lz_interface_entry(iface, i);
        if (lz_interface_entry_kind(entry) == LZ_IFACE_FN && strcmp(lz_interface_entry_name(entry), name) == 0) {
            sig = lz_interface_entry_sig(entry);
        }
    }
    LZ_CHECK(sig != NULL);
    if (sig != NULL) {
        for (size_t i = 0; i < lz_interface_sig_type_param_count(sig); i++) {
            put(&dump, i == 0 ? "<" : ", ");
            put(&dump, lz_interface_sig_type_param_name(sig, i));
            if (i + 1 == lz_interface_sig_type_param_count(sig)) {
                put(&dump, ">");
            }
        }
        put_sig(&dump, sig);
        put(&dump, lz_interface_sig_result_declared(sig) ? " declared" : " inferred");
    }
    const char *actual = dump.data != NULL ? dump.data : "";
    if (strcmp(actual, expected) != 0) {
        fprintf(stderr, "program:\n%s\nexpected: %s\nactual:   %s\n", text, expected, actual);
        lz_test_failures++;
    }
    lz_buffer_free(&dump);
    lz_chunk_free(&vm.heap, &chunk);
    lz_vm_release_roots(&vm);
    lz_vm_free(&vm);
    lz_buffer_free(&out);
    lz_buffer_free(&err);
}

static void test_interface_function_types(void) {
    check_fn_sig("pub fn apply(f: fn(int, string) -> bool, x: int) -> int => 1\n", "apply",
                 "(f: fn(int, string) -> bool, x: int) -> int declared");
    check_fn_sig("pub type Pred = fn(int) -> bool\npub fn pick(p: Pred, xs: int[]) -> int[] => xs\n", "pick",
                 "(p: fn(int) -> bool, xs: int[]) -> int[] declared");
    /* a function type with a result is parenthesised where something follows it */
    check_fn_sig("pub fn maybe(f: (fn(int) -> int)?, gs: (fn(int))[]) -> (fn(int) -> int)[] => []\n", "maybe",
                 "(f: nil | (fn(int) -> int), gs: fn(int)[]) -> (fn(int) -> int)[] declared");
    check_fn_sig("pub fn make() -> fn(int) -> int => fn(n) => n\n", "make", "() -> fn(int) -> int declared");
}

static void test_interface_generics(void) {
    check_fn_sig("pub fn first<T>(xs: T[]) -> T => xs[0]\n", "first", "<T>(xs: T[]) -> T declared");
    check_fn_sig("pub fn map<T, U>(xs: T[], f: fn(T) -> U) -> U[] {\n  let out: U[] = []\n  return out\n}\n", "map",
                 "<T, U>(xs: T[], f: fn(T) -> U) -> U[] declared");
    check_fn_sig("pub fn or_else<T>(x: T?, d: T) -> T => d\n", "or_else", "<T>(x: T?, d: T) -> T declared");
    check_fn_sig("pub fn plain(x: int) -> int => x\n", "plain", "(x: int) -> int declared");
    /* a generic with no declared result: inside the body T is any, so the result is not worked out as T */
    check_fn_sig("pub fn id<T>(x: T) => x\n", "id", "<T>(x: T) -> any inferred");
}

static void test_interface_inferred_results(void) {
    check_fn_sig("pub fn one() => 1\n", "one", "() -> int inferred");
    check_fn_sig("pub fn two(c) {\n  if c { return 1 }\n  return \"s\"\n}\n", "two", "(c: any) -> int | string inferred");
    check_fn_sig("pub fn maybe(c) {\n  if c { return 1 }\n}\n", "maybe", "(c: any) -> int | nil inferred");
    check_fn_sig("pub fn nothing() {\n  println(1)\n}\n", "nothing", "() -> nil inferred");
    check_fn_sig("pub fn fact(n) {\n  if n <= 1 { return 1 }\n  return n * fact(n - 1)\n}\n", "fact", "(n: any) -> any inferred");
    check_fn_sig("pub fn unknown(x) => x\n", "unknown", "(x: any) -> any inferred");
    check_fn_sig("pub fn declared() -> number => 1\n", "declared", "() -> number declared");
    check_fn_sig("pub fn list() => [1, 2]\n", "list", "() -> int[] inferred");
}

static void test_interface_cases(void) {
    /* 1: every kind */
    check_interface(
        "pub fn add(a: int, b: int) -> int => a + b\n"
        "pub fn greet(name, greeting = \"hi\", *rest: int) {}\n"
        "pub const MAX = 10\n"
        "pub const counter = 0\n"
        "pub const names: string[] = []\n"
        "pub type Id = int\n"
        "pub class Point {\n    pub x: int\n    pub y: int = 0\n    pub fn sum(self) => self.x + self.y\n}\n"
        "pub trait Shape {\n    fn area(self)\n    fn label(self) => \"shape\"\n}\n"
        "pub error NotFound : Error {\n    pub needed: int\n}\n"
        "fn hidden() => 1\n",
        
        "fn add(a: int, b: int) -> int\n"
        "fn greet(name: any, greeting: any =, *rest: int) -> nil\n"
        "const MAX: int\n"
        "const counter: int\n"
        "const names: string[]\n"
        "type Id: int\n"
        "class Point\n"
        "  init init(x: int, y: int =) -> nil\n"
        "  field x: int\n"
        "  field y: int =\n"
        "  method sum() -> any\n"
        "trait Shape\n"
        "  method area() -> any required\n"
        "  method label() -> string\n"
        "error NotFound : Error\n"
        "  init init(message: string, needed: int) -> nil\n"
        "  field needed: int\n");
    /* 2: private field and method stay out, the class stays pub */
    check_interface(
        "pub class Counter {\n    pub name: string\n    count: int = 0\n    fn bump(self) { self.count += 1 }\n"
        "    pub fn total(self) -> int => self.count\n    const LIMIT = 3\n}\n",
        
        "class Counter\n"
        "  init init(name: string) -> nil\n"
        "  field name: string\n"
        "  method total() -> int\n");
    /* 3: explicit init */
    check_interface(
        "pub class Temp {\n    pub celsius: float\n    kelvin: float\n    fn init(self) { self.kelvin = self.celsius + 273.15 }\n"
        "    pub get k(self) -> float => self.kelvin\n}\n",
        
        "class Temp\n"
        "  init init(celsius: float) -> nil explicit\n"
        "  field celsius: float\n"
        "  get k() -> float\n");
    /* 4: no init, some fields private with defaults */
    check_interface(
        "pub class Config {\n    pub host: string\n    secret: string = \"x\"\n    pub port: int = 80\n    retries: int = 3\n}\n",
        
        "class Config\n"
        "  init init(host: string, port: int =) -> nil\n"
        "  field host: string\n"
        "  field port: int =\n");
    /* 5: parent and pub read, operator, static, const */
    check_interface(
        "class Animal {\n    pub name: string\n}\n"
        "pub class Dog : Animal {\n    pub read age: int\n    pub breed: string = \"mixed\"\n"
        "    pub const LEGS = 4\n    pub fn origin() => 0\n    pub override fn to_string(self) => self.name\n"
        "    pub operator ==(self, other) => true\n}\n",
        
        "class Dog : Animal\n"
        "  init init(name: string, age: int, breed: string =) -> nil\n"
        "  field read age: int\n"
        "  field breed: string =\n"
        "  const LEGS: int\n"
        "  static origin() -> int\n"
        "  method override to_string() -> any\n"
        "  operator ==(other: any) -> bool\n");
    /* 6: trait with a parent trait, class using a trait, error with a parent error */
    check_interface(
        "trait Named {\n    get label(self)\n}\n"
        "pub trait Shape : Named {\n    fn area(self)\n}\n"
        "pub class Sq : Shape {\n    pub side: int\n    pub fn area(self) => self.side\n    pub get label(self) => \"sq\"\n}\n"
        "error Base : Error {\n    pub code: int\n}\n"
        "pub error Child : Base {\n    pub extra: string = \"\"\n}\n",
        
        "trait Shape + Named\n"
        "  method area() -> any required\n"
        "class Sq + Shape\n"
        "  init init(side: int) -> nil\n"
        "  field side: int\n"
        "  method area() -> any\n"
        "  get label() -> string\n"
        "error Child : Base\n"
        "  init init(message: string, code: int, extra: string =) -> nil\n"
        "  field extra: string =\n");
    /* pub changes nothing at run time */
    check_runs("pub fn add(a: int, b: int) -> int => a + b\npub const MAX = 10\npub class P { pub x: int }\n"
               "pub trait T { fn f(self) => 1 }\npub type Id = int\npub error E : Error\nprintln(add(P(1).x, MAX))\n",
               "11\n");
    check_runs("let pub = 4\npub = pub + 1\nprintln(pub, pub is int)\n", "5 true\n");
    /* `pub let` is refused: only what stays the same is shared */
    check_pub_let("pub let limit = 10\nprintln(limit)\n", NULL, "pub const limit = 10\nprintln(limit)\n");
    check_pub_let("pub let names: string[] = []\n", NULL, "pub const names: string[] = []\n");
    check_pub_let("pub let limit = 10\nfn raise() { limit += 1 }\nraise()\n",
                  "keep it private and share it through a function, for example `pub fn limit() => limit`", NULL);
    check_pub_let("pub let limit = 10\nlimit = 20\n",
                  "keep it private and share it through a function, for example `pub fn limit() => limit`", NULL);
    check_pub_let("pub let limit\n",
                  "keep it private and share it through a function, for example `pub fn limit() => limit`", NULL);
    /* a name inside a string is not an assignment */
    check_pub_let("pub let limit = 10\nprintln(\"limit = 1\")\n", NULL, "pub const limit = 10\nprintln(\"limit = 1\")\n");
    /* 2: pub in the wrong place */
    check_error("fn f() {\n    pub fn g() => 1\n}\n", "`pub` only works on top-level declarations",
                "this is inside a function", "", true);
    check_error("if true {\n    pub let x = 1\n}\n", "`pub` only works on top-level declarations",
                "this is inside a block", "", true);
    check_error("pub extend int {\n    pub fn d(self) => 1\n}\n", "'pub' cannot be used on 'extend'",
                "an extension stays inside the module that writes it",
                "other modules never see it, so it cannot change how they use int", true);
    check_error("pub import foo\n", "`pub` cannot go in front of `import`",
                "`import` brings names in; it does not declare one to share", "", true);
    check_error("pub switch 1 {\n    case 1 => 1\n    else => 2\n}\n", "`pub` cannot go in front of `switch`",
                "`switch` runs code, it does not declare a name", "", true);
    check_error("pub println(1)\n", "`pub` must come before a declaration", "this `pub` is not followed by a declaration",
                "", true);
    check_error("pub pub fn f() => 1\n", "`pub` is written twice", "`pub` is already there", "", true);
    /* 4: pub must not leak a type that is not pub */
    check_error("class H { pub n: int }\npub fn f(h: H) => 1\n", "`f` is pub, but its signature uses `H`, which is not",
                "`H` is not `pub`", "code outside could not name this type", true);
    check_error("class H { pub n: int }\npub fn f() -> H => H(1)\n", "`f` is pub, but its signature uses `H`, which is not",
                "`H` is not `pub`", "code outside could not name this type", true);
    check_error("class H { pub n: int }\npub class B {\n    pub inner: H\n}\n",
                "`B.inner` is pub, but its type uses `H`, which is not", "`H` is not `pub`",
                "code outside could not name this type", true);
    check_error("class H { pub n: int }\npub class B {\n    pub fn m(self, h: H) => 1\n}\n",
                "`B.m` is pub, but its signature uses `H`, which is not", "`H` is not `pub`",
                "code outside could not name this type", true);
    check_error("type Id = int\npub fn f(x: Id) => x\n", "`f` is pub, but its signature uses `Id`, which is not",
                "`Id` is not `pub`", "code outside could not name this type", true);
    check_error("trait T { fn f(self) }\npub fn g(x: T) => 1\n", "`g` is pub, but its signature uses `T`, which is not",
                "`T` is not `pub`", "code outside could not name this type", true);
    /* private members may use any type, and builtin or generic types are fine */
    check_interface("class H { pub n: int }\npub class B {\n    inner: H = H(1)\n    fn m(self, h: H) -> H => h\n"
                    "    pub fn k(self, xs: int[], d: dict) -> string? => nil\n}\n",
                    "class B\n  init init() -> nil\n  method k(xs: int[], d: dict) -> string | nil\n");
    /* 7: nothing pub: an empty interface */
    check_interface("fn a() => 1\nlet b = 2\nclass C { pub x: int }\n", "");
}

int main(void) {
    test_interface_cases();
    test_interface_function_types();
    test_interface_generics();
    test_interface_inferred_results();
    return LZ_TEST_RESULT();
}
