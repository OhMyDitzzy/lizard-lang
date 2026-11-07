#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "lizard/builtin.h"
#include "lizard/compiler.h"
#include "lizard/dict.h"
#include "lizard/debug.h"
#include "lizard/suggest.h"
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

/* Fuel or heap limit of 0 means unlimited. */
static void run_with(Program *program, const char *text, uint64_t fuel, size_t heap_limit) {
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

static void run(Program *program, const char *text) {
    run_with(program, text, 0, 0);
}

static void finish(Program *program) {
    LZ_CHECK(program->vm.stack_count == 0);
    lz_chunk_free(&program->vm.heap, &program->chunk);
    lz_vm_release_roots(&program->vm);
    LZ_CHECK(program->vm.heap.live_objects == 0); /* no leaked dict, however the script ended */
    LZ_CHECK(program->vm.heap.live_bytes == 0);   /* entry and slot buffers are accounted for */
    lz_vm_free(&program->vm);
    lz_buffer_free(&program->out);
    lz_buffer_free(&program->err);
}

static void check_output(const char *text, const char *expected) {
    Program program;
    run(&program, text);
    if (program.compile_status != LZ_OK) {
        fprintf(stderr, "does not compile:\n%s", text);
        for (size_t i = 0; i < program.diagnostics.count; i++) {
            fprintf(stderr, "  diagnostic: %s\n", program.diagnostics.items[i].message);
        }
    }
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
    run(&program, text);
    bool ok = false;
    for (size_t i = 0; i < program.diagnostics.count; i++) {
        ok = ok || strstr(program.diagnostics.items[i].message, message) != NULL;
    }
    ok = ok && program.compile_status == LZ_ERR_COMPILE;
    if (!ok) {
        fprintf(stderr, "program:\n%s\nexpected a compile error with [%s]; got status %d\n", text,
                message, (int)program.compile_status);
        for (size_t i = 0; i < program.diagnostics.count; i++) {
            fprintf(stderr, "  diagnostic: %s\n", program.diagnostics.items[i].message);
        }
    }
    LZ_CHECK(ok);
    finish(&program);
}

/* A run-time failure with this message and this suggestion (NULL = none). */
static void check_run_error(const char *text, const char *message, const char *suggestion) {
    Program program;
    run(&program, text);
    LZ_CHECK(program.compile_status == LZ_OK);
    bool found = program.run_status != LZ_OK && strstr(program.vm.error, message) != NULL;
    if (found && suggestion != NULL) {
        found = program.vm.error_suggestions.count > 0 &&
                strcmp(program.vm.error_suggestions.names[0], suggestion) == 0;
    }
    if (!found) {
        fprintf(stderr,
                "program:\n%s\nexpected [%s] with suggestion [%s]; got %d [%s] (suggestions: %zu)\n",
                text, message, suggestion != NULL ? suggestion : "none", (int)program.run_status,
                program.vm.error, program.vm.error_suggestions.count);
        for (size_t i = 0; i < program.vm.error_suggestions.count; i++) {
            fprintf(stderr, "  suggestion: %s\n", program.vm.error_suggestions.names[i]);
        }
    }
    LZ_CHECK(found);
    finish(&program);
}

static void check_runs_out_of_fuel(const char *text, uint64_t fuel) {
    Program program;
    run_with(&program, text, fuel, 0);
    LZ_CHECK(program.compile_status == LZ_OK);
    bool ok = program.run_status == LZ_ERR_FUEL &&
              strstr(program.vm.error, "instruction limit") != NULL;
    if (!ok) {
        fprintf(stderr, "program:\n%s\nexpected the fuel to run out; got %d [%s]\n", text,
                (int)program.run_status, program.vm.error);
    }
    LZ_CHECK(ok);
    finish(&program);
}

static void test_literals(void) {
    check_output("let d = {\"ana\": 20, \"budi\": 31}\nprintln(d)\n",
                 "{\"ana\": 20, \"budi\": 31}\n");
    check_output("println({})\n", "{}\n");
    /* Keys are expressions; there is no name-to-string shortcut. */
    check_output("let k = \"k\"\nprintln({k: 1})\n", "{\"k\": 1}\n");
    check_output("println({1 + 1: \"two\"})\n", "{2: \"two\"}\n");
    check_output("let d = {\n  \"a\": 1,\n  \"b\": 2,\n}\nprintln(d)\n",
                 "{\"a\": 1, \"b\": 2}\n");
    check_output("println({\"a\": 1,})\n", "{\"a\": 1}\n");
    /* Values nest. */
    check_output("println({\"xs\": [1, 2], \"inner\": {\"y\": true}})\n",
                 "{\"xs\": [1, 2], \"inner\": {\"y\": true}}\n");
    /* `{` at the start of a statement is still a block. */
    check_output("{\n let x = 1\n println(x)\n}\n", "1\n");
    /* A dict literal in expression position is a dict. */
    check_output("println([{\"a\": 1}])\n", "[{\"a\": 1}]\n");
    check_output("println({\"a\": 1}[\"a\"])\n", "1\n");
}

static void test_keys(void) {
    check_output("println({1: \"int\", true: \"bool\", \"1\": \"string\"})\n",
                 "{1: \"int\", true: \"bool\", \"1\": \"string\"}\n");
    check_output("let d = {1: \"a\", \"1\": \"b\"}\nprintln(d[1], d[\"1\"])\n", "a b\n");
    check_output("let d = {true: 1}\nprintln(d[true], d.has(true), d.has(1))\n", "1 true false\n");
    /* Overwriting keeps the position of the key. */
    check_output("let d = {\"a\": 1, \"b\": 2}\nd[\"a\"] = 9\nprintln(d, d.keys())\n",
                 "{\"a\": 9, \"b\": 2} [\"a\", \"b\"]\n");
    check_compile_error("println({1.5: \"x\"})\n", "a float cannot be a dict key");
    check_compile_error("println({[]: \"x\"})\n", "a array cannot be a dict key");
    check_compile_error("println({{}: \"x\"})\n", "a dict cannot be a dict key");
    check_compile_error("println({nil: \"x\"})\n", "a nil cannot be a dict key");
    check_compile_error("println({\"a\": 1, \"a\": 2, \"a\": 3})\n",
                        "duplicate key \"a\" in the dict literal");
    check_compile_error("println({1: 1, true: 2, 1: 3})\n",
                        "duplicate key 1 in the dict literal");
    check_run_error("let k = 1.5\nlet d = {k: \"x\"}\n", "a float cannot be a dict key", NULL);
}

static void test_access(void) {
    check_output("let d = {\"a\": 1}\nprintln(d[\"a\"])\n", "1\n");
    check_output("let d = {\"a\": 1}\nd[\"b\"] = 2\nd[\"a\"] = 3\nprintln(d)\n",
                 "{\"a\": 3, \"b\": 2}\n");
    check_output("let d = {}\nd[\"x\"] = 1\nprintln(d)\n", "{\"x\": 1}\n");
    check_output("let d = {\"a\": 1}\nd[\"a\"] += 2\nprintln(d[\"a\"])\n", "3\n");
    check_output("let d = {\"a\": 1}\nlet k = \"a\"\nd[k] += 1\nprintln(d[k])\n", "2\n");
    check_run_error("let d = {\"ana\": 1}\nprintln(d[\"dewi\"])\n", "key \"dewi\" not found", NULL);
    check_run_error("let d = {\"ana\": 1}\nd[\"dewi\"] += 1\n",
                    "key \"dewi\" not found (use d.get(k, 0) + v)", NULL);
    check_run_error("let d = {\"ana\": 1}\nlet k = nil\nprintln(d[k])\n",
                    "a nil cannot be a dict key", NULL);
    check_run_error("let d = {\"ana\": 1}\nlet k = [1]\nd[k] = 2\n",
                    "a array cannot be a dict key", NULL);
}

static void test_suggestions(void) {
    check_run_error("let d = {\"ana\": 20, \"andi\": 3}\nprintln(d[\"ane\"])\n",
                    "key \"ane\" not found", "ana");
    check_run_error("let d = {\"ana\": 20, \"budi\": 31}\nprintln(d[\"bdi\"])\n",
                    "key \"bdi\" not found", "budi");
    /* An int key never suggests a string. */
    check_run_error("let d = {1: 2}\nprintln(d[7])\n", "key 7 not found", NULL);
}

static void test_methods(void) {
    check_output("let d = {\"a\": 1, \"b\": 2}\nprintln(d.len(), len(d), d.is_empty())\n",
                 "2 2 false\n");
    check_output("println({}.len(), len({}), {}.is_empty())\n", "0 0 true\n");
    check_output("let d = {\"a\": 1, \"b\": 2}\nprintln(d.keys(), d.values())\n",
                 "[\"a\", \"b\"] [1, 2]\n");
    check_output("let d = {\"a\": 1, \"b\": 2}\nprintln(d.items())\n",
                 "[[\"a\", 1], [\"b\", 2]]\n");
    check_output("let d = {\"a\": 1}\nprintln(d.get(\"a\"), d.get(\"z\"), d.get(\"z\", 9))\n",
                 "1 nil 9\n");
    check_output("let d = {\"a\": 1}\nprintln(d.has(\"a\"), d.has(\"z\"), d.has(1))\n",
                 "true false false\n");
    check_output("let d = {\"a\": 1, \"b\": 2}\nprintln(d.remove(\"a\"), d)\n",
                 "1 {\"b\": 2}\n");
    check_output("let d = {\"a\": 1}\nd.clear()\nprintln(d.len(), d)\n", "0 {}\n");
    check_output("let d = {\"a\": 1}\nlet c = d.copy()\nc[\"b\"] = 2\nprintln(d, c)\n",
                 "{\"a\": 1} {\"a\": 1, \"b\": 2}\n");
    check_output("let a = {\"x\": 1, \"y\": 2}\nlet b = {\"y\": 20, \"z\": 3}\n"
                 "println(a.merge(b), a, b)\n",
                 "{\"x\": 1, \"y\": 20, \"z\": 3} {\"x\": 1, \"y\": 2} {\"y\": 20, \"z\": 3}\n");
    /* The copies are arrays of their own: changing one does not change the dict. */
    check_output("let d = {\"a\": 1}\nlet ks = d.keys()\npush(ks, \"b\")\nprintln(d, ks)\n",
                 "{\"a\": 1} [\"a\", \"b\"]\n");
    check_run_error("let d = {\"a\": 1}\nd.remove(\"z\")\n", "key \"z\" not found", NULL);
    check_run_error("let d = {\"a\": 1}\nd.merge(1)\n", "merge(): expected a dict, got int", NULL);
    check_run_error("let d = {\"a\": 1}\nd.get(1.5)\n", "a float cannot be a dict key", NULL);
    /* A dict has no method of its own for these. */
    check_run_error("println(len({\"a\": 1}, 2))\n", "len(): wrong number of arguments", NULL);
}

static void test_order(void) {
    check_output("let d = {\"c\": 1, \"a\": 2, \"b\": 3}\nprintln(d.keys())\n",
                 "[\"c\", \"a\", \"b\"]\n");
    /* Delete and add again: the key moves to the end. */
    check_output("let d = {\"a\": 1, \"b\": 2, \"c\": 3}\nd.remove(\"a\")\nd[\"a\"] = 1\n"
                 "println(d.keys())\n",
                 "[\"b\", \"c\", \"a\"]\n");
    check_output("let d = {}\nfor i in 0..5 { d[i] = i }\nprintln(d.keys())\n",
                 "[0, 1, 2, 3, 4]\n");
    /* Ten thousand entries stay in order through growth and mass deletion. */
    check_output("let d = {}\nfor i in 0..10000 { d[i] = i }\n"
                 "let first = d.keys()[0]\nlet last = d.keys()[-1]\n"
                 "for i in 0..10000 { if i % 2 == 0 { d.remove(i) } }\n"
                 "println(d.len(), first, last, d.keys()[0], d.keys()[-1])\n",
                 "5000 0 9999 1 9999\n");
    check_output("let d = {}\nfor i in 0..100 { d[i] = i }\nfor i in 0..100 { d.remove(i) }\n"
                 "for i in 0..100 { d[i] = i }\n"
                 "let ok = true\nlet i = 0\nfor k in d { if k != i { ok = false }\n i += 1 }\n"
                 "println(ok, d.len())\n",
                 "true 100\n");
}

static void test_iteration(void) {
    check_output("let d = {\"a\": 1, \"b\": 2}\nfor k in d { println(k) }\n", "a\nb\n");
    check_output("let d = {\"a\": 1, \"b\": 2}\nfor k, v in d { println(k, v) }\n", "a 1\nb 2\n");
    check_output("for k in ({}) { println(k) }\n", "");
    /* An array still counts rounds in the two-variable form. */
    check_output("for i, x in [\"a\", \"b\"] { println(i, x) }\n", "0 a\n1 b\n");
    check_output("let d = {\"a\": 1}\nfor k, v in d { println(k, v) }\n", "a 1\n");
    /* A value may change during the walk; the size may not. */
    check_output("let d = {\"a\": 1, \"b\": 2}\nfor k in d { d[k] = d[k] * 10 }\nprintln(d)\n",
                 "{\"a\": 10, \"b\": 20}\n");
    check_run_error("let d = {\"a\": 1}\nfor k in d { d[\"b\"] = 2 }\n",
                    "dict changed size during iteration", NULL);
    check_run_error("let d = {\"a\": 1, \"b\": 2}\nfor k in d { d.remove(k) }\n",
                    "dict changed size during iteration", NULL);
    check_run_error("let d = {\"a\": 1}\nfor k in d { d.clear() }\n",
                    "dict changed size during iteration", NULL);
    /* Re-adding a key in the same round keeps the size, so it is allowed. */
    check_output("let d = {\"a\": 1}\nfor k in d {\n d.remove(k)\n d[k] = 2\n}\nprintln(d)\n",
                 "{\"a\": 2}\n");
    check_output("let d = {\"a\": 1, \"b\": 2}\nlet n = 0\nfor k in d { n += 1\n if n == 1 { break } }\n"
                 "println(n)\n",
                 "1\n");
    check_output("let d = {\"a\": 1, \"b\": 2}\nfor k in d { if k == \"a\" { continue }\n println(k) }\n",
                 "b\n");
    /* An annotated loop variable is checked against the keys. */
    check_output("let d = {\"a\": 1}\nfor k: string in d { println(k) }\n", "a\n");
    check_run_error("let d = {\"a\": 1}\nfor k: int in d { println(k) }\n",
                    "type error: expected int, got string", NULL);
    check_run_error("let d = {\"a\": 1}\nfor k, v: string in d { println(k, v) }\n",
                    "type error: expected string, got int", NULL);
    /* A lambda in the body keeps that round's key. */
    check_output("let d = {\"a\": 1, \"b\": 2}\nlet gs = []\nfor k in d { push(gs, fn() { return k }) }\n"
                 "println(gs[0](), gs[1]())\n",
                 "a b\n");
    /* A dict is not iterable in the middle of another walk, but nesting is fine. */
    check_output("let d = {\"a\": 1}\nlet e = {\"x\": 1, \"y\": 2}\nlet n = 0\n"
                 "for k in d { for m in e { n += 1 } }\nprintln(n)\n",
                 "2\n");
}

static void test_display_and_templates(void) {
    check_output("println({\"a\": \"b\"})\n", "{\"a\": \"b\"}\n");
    check_output("let d = {\"a\": 1}\nprintln(\"{d}\")\n", "{d}\n");
    check_output("let d = {\"a\": 1}\nprintln($\"{d}\")\n", "{\"a\": 1}\n");
    check_output("let d = {\"a\": 1}\nprintln($\"{d=}\")\n", "d={\"a\": 1}\n");
    check_output("let d = {\"a\": 1}\nprintln(d.to_debug())\n", "{\"a\": 1}\n");
    check_output("let d = {\"a\": 1}\nprintln($\"{d:s}\")\n", "{\"a\": 1}\n");
    /* A dict literal at the very start of a template needs a space or parentheses. */
    check_output("println($\"{ {\"a\": 1}[\"a\"] }\")\n", "1\n");
    check_output("println($\"{({ \"a\": 1 })[\"a\"]}\")\n", "1\n");
    check_output("println($\"{{\")\n", "{\n");
    /* Printing a deeply nested structure does not overflow the C stack. */
    check_output("let d = {}\nlet cur = d\nfor i in 0..2000 {\n let next = {}\n cur[\"n\"] = next\n"
                 " cur = next\n}\nprintln(len(d))\n",
                 "1\n");
    check_output("let xs = []\nlet d = {\"xs\": xs}\nlet cur = xs\n"
                 "for i in 0..2000 { let next = []\n push(cur, next)\n cur = next }\n"
                 "println(len(d[\"xs\"]))\n",
                 "1\n");
}

static void test_equality(void) {
    check_output("println({\"a\": 1} == {\"a\": 1}, {\"a\": 1} != {\"a\": 2})\n", "true true\n");
    /* The order does not matter for equality. */
    check_output("println({\"a\": 1, \"b\": 2} == {\"b\": 2, \"a\": 1})\n", "true\n");
    check_output("println({} == {}, {} == {\"a\": 1}, {\"a\": 1} == 1)\n", "true false false\n");
    check_output("println({\"a\": [1, {\"b\": 2}]} == {\"a\": [1, {\"b\": 2}]})\n", "true\n");
    check_output("println({\"a\": 1} == {\"a\": 1.0}, {\"a\": 1} == {\"a\": 2.0})\n", "true false\n");
    check_output("let d = {\"a\": 1}\nlet e = d\nprintln(d == e, d != e)\n", "true false\n");
    /* Truthiness is like an array: only nil and false are falsy. */
    check_output("println(if {} { \"yes\" } else { \"no\" })\n", "yes\n");
}

static void test_annotations(void) {
    check_output("let d: dict[string, int] = {\"a\": 1}\nprintln(d[\"a\"])\n", "1\n");
    check_output("let d: dict = {\"a\": 1}\nprintln(d.len())\n", "1\n");
    check_output("fn f(d: dict[string, int]) -> int { return d[\"a\"] }\nprintln(f({\"a\": 7}))\n",
                 "7\n");
    check_compile_error("let d: dict[string, int] = {\"a\": \"x\"}\n", "type mismatch");
    check_compile_error("let d: dict[string, int] = {1: 1}\n", "type mismatch");
    check_compile_error("let d: dict[string, int] = {\"a\": 1}\nd[\"b\"] = \"no\"\n",
                        "type mismatch: expected int, got \"no\"");
    check_compile_error("let d: dict[int, int]\n", "needs an initial value");
    check_compile_error("let d: dict[string] = {}\n", "dict[...] expects a key type and a value type");
    check_compile_error("let d: dict[string, int, int] = {}\n",
                        "expected ']' after the dict types");
    check_compile_error("let d: dict[int[], int] = {}\n", "cannot be a dict key");
    /* A misspelled type suggests the right one. */
    check_compile_error("let d: dikt = {}\n", "unknown type 'dikt'");
    /* A dict is not callable. */
    check_compile_error("let d: dict = {}\nd()\n", "a dict is not callable");
    check_run_error("let d = {}\nd()\n", "can only call functions", NULL);
    /* Nor is it a slice. */
    check_compile_error("let d: dict = {\"a\": 1}\nprintln(d[1:2])\n", "cannot use a dict here");
    /* `any[]` is still an array. */
    check_output("let xs: any[] = [1]\nprintln(xs)\n", "[1]\n");
}

static void test_cycles(void) {
    check_run_error("let d = {}\nd[\"self\"] = d\n",
                    "a value cannot contain itself", NULL);
    check_run_error("let d = {}\nlet e = {\"d\": d}\nd[\"e\"] = e\n",
                    "a value cannot contain itself", NULL);
    check_run_error("let xs = []\nlet d = {\"xs\": xs}\npush(xs, d)\n",
                    "a value cannot contain itself", NULL);
    check_run_error("let d = {}\nlet e = d\nlet f = {}\nf[\"x\"] = e\ne[\"y\"] = f\n",
                    "a value cannot contain itself", NULL);
    /* A shared (not cyclic) dict is fine. */
    check_output("let inner = {\"a\": 1}\nlet d = {\"x\": inner, \"y\": inner}\nprintln(d)\n",
                 "{\"x\": {\"a\": 1}, \"y\": {\"a\": 1}}\n");
}

static void test_builtin_edge_cases(void) {
    /* A wrong key kind on every method. */
    check_run_error("let d = {}\nd.has(1.5)\n", "a float cannot be a dict key", NULL);
    check_run_error("let d = {}\nd.remove(nil)\n", "a nil cannot be a dict key", NULL);
    /* A method that fails leaves the dict as it was. */
    check_output("let d = {\"a\": 1}\nlet ok = false\n"
                 "if d.get(\"a\") == 1 { ok = true }\nprintln(ok, d)\n",
                 "true {\"a\": 1}\n");
    /* Len of a dict is the entry count, not the buffer size. */
    check_output("let d = {}\nfor i in 0..100 { d[i] = i }\nfor i in 0..99 { d.remove(i) }\n"
                 "println(len(d), d.keys())\n",
                 "1 [99]\n");
    /* Copy and merge keep the type restrictions. */
    check_run_error("let d: dict[string, int] = {\"a\": 1}\nlet c = d.copy()\nc[\"b\"] = \"x\"\n",
                    "type error: dict of int cannot hold string", NULL);
}

static void test_fuel(void) {
    check_runs_out_of_fuel("let d = {}\nfor i in 0..1000 { d[i] = i }\n"
                           "for let i = 0; true; i += 1 { d.copy() }\n",
                           20000);
    check_runs_out_of_fuel("let d = {}\nfor i in 0..100 { d[i] = i }\n"
                           "for let i = 0; true; i += 1 { d.keys() }\n",
                           20000);
    check_runs_out_of_fuel("let d = {}\nfor i in 0..100 { d[i] = i }\n"
                           "for let i = 0; true; i += 1 { d == d.copy() }\n",
                           40000);
    check_runs_out_of_fuel("let d = {}\nfor i in 0..100 { d[i] = i }\n"
                           "for let i = 0; true; i += 1 { d.merge(d) }\n",
                           40000);
}

static void test_heap(void) {
    /* A dict is charged to the heap, so a small limit stops it. */
    Program program;
    run_with(&program, "let d = {}\nfor i in 0..100000 { d[i] = i }\n", 0, 8 * 1024);
    LZ_CHECK(program.compile_status == LZ_OK);
    bool failed = program.run_status == LZ_ERR_HEAP_LIMIT;
    if (!failed) {
        fprintf(stderr, "expected the heap limit to stop the dict; got %d [%s]\n",
                (int)program.run_status, program.vm.error);
    }
    LZ_CHECK(failed);
    finish(&program);

    /* The dict and its buffers come back when the script ends. */
    check_output("let d = {}\nfor i in 0..1000 { d[i] = [i, i + 1] }\n"
                 "for i in 0..1000 { d.remove(i) }\nprintln(d.len())\n",
                 "0\n");
    check_output("let d = {\"a\": [1, 2]}\nlet c = d.copy()\nprintln(c)\n", "{\"a\": [1, 2]}\n");
    check_output("fn f() { let d = {\"a\": 1}\n return d[\"a\"] }\nprintln(f())\n", "1\n");
    /* A closure keeps the dict it captured alive. */
    check_output("let d = {\"a\": 1}\nlet f = fn() { return d[\"a\"] }\nprintln(f())\n", "1\n");
}

static void test_callbacks(void) {
    check_output("fn add(v: int, d: dict[string, int]) { d[\"total\"] += v }\n"
                 "let d = {\"total\": 0}\nadd(3, d)\nadd(4, d)\nprintln(d[\"total\"])\n",
                 "7\n");
    check_output("let d = {\"a\": 1, \"b\": 2}\n"
                 "fn double(v) { return v * 2 }\n"
                 "println(d.keys().map(fn(k) => double(d[k])))\n",
                 "[2, 4]\n");
    check_output("let d = {\"a\": 1}\nfn make() { return fn() => {\"got\": d[\"a\"]} }\n"
                 "println(make()()[\"got\"])\n",
                 "1\n");
    check_output("let d = {}\nfn fill(n: int) { for i in 0..n { d[i] = i } }\nfill(500)\n"
                 "println(d.len())\n",
                 "500\n");
}

static void test_parser_edges(void) {
    /* `fn() => {"a": 1}` is a dict; `=> {` with a statement is still a block error. */
    check_output("let f = fn() => {\"a\": 1}\nprintln(f()[\"a\"])\n", "1\n");
    check_output("let f = fn() => {}\nprintln(f())\n", "{}\n");
    check_compile_error("let f = fn() => {\n let x = 1\n}\n",
                        "a block body is written without '=>'");
    check_compile_error("let f = fn() => {\n println(1)\n}\n",
                        "a block body is written without '=>'");
    /* A dict literal after `in` needs parentheses: `{` starts the body there. */
    check_compile_error("for k in {\"a\": 1} { println(k) }\n", "expected an expression after 'in'");
    check_output("for k in ({\"a\": 1}) { println(k) }\n", "a\n");
    check_compile_error("println({\"a\" 1})\n", "expected ':' after the key in the dict literal");
    check_compile_error("println({\"a\": 1)\n", "expected '}' after the dict literal");
    check_compile_error("println({\"a\": })\n", "expected a value after ':'");
    check_compile_error("println({\"a\": 1\n", "expected '}' after the dict literal");
    check_compile_error("println({\"a\": 1 \"b\": 2})\n", "expected '}' after the dict literal");
    /* A newline inside the literal is fine, in every position. */
    check_output("let d = {\n \"a\": 1\n}\nprintln(d)\n", "{\"a\": 1}\n");
    check_output("let d = {\"a\":\n 1}\nprintln(d)\n", "{\"a\": 1}\n");
    /* A dict literal as a call argument, as a condition and as a value. */
    check_output("fn f(d) { return d.len() }\nprintln(f({\"a\": 1, \"b\": 2}))\n", "2\n");
    check_output("println(if {\"a\": 1} { \"t\" } else { \"f\" })\n", "t\n");
}

static void test_disassembly(void) {
    Program program;
    run(&program, "let d = {\"a\": 1}\nfor k, v in d { println(k, v) }\n");
    LZ_CHECK(program.compile_status == LZ_OK);
    LzBuffer out = {0};
    LzWriter writer = lz_writer_from_buffer(&out);
    lz_disassemble_chunk(&program.chunk, "test", &program.source, &writer);
    LZ_CHECK(strstr(out.data, "OP_DICT") != NULL);
    LZ_CHECK(strstr(out.data, "OP_ITER_PAIR") != NULL);
    lz_buffer_free(&out);
    finish(&program);
}

int main(void) {
    test_literals();
    test_keys();
    test_access();
    test_suggestions();
    test_methods();
    test_order();
    test_iteration();
    test_display_and_templates();
    test_equality();
    test_annotations();
    test_cycles();
    test_builtin_edge_cases();
    test_fuel();
    test_heap();
    test_callbacks();
    test_parser_edges();
    test_disassembly();
    return LZ_TEST_RESULT();
}
