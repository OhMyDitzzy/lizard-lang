#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lizard/builtin.h"
#include "lizard/compiler.h"
#include "test_util.h"

static uint64_t state = 0x2545F4914F6CDD1Dull;

static uint64_t next_random(void) {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return state;
}

static uint32_t below(uint32_t limit) {
    return (uint32_t)(next_random() % limit);
}

static bool discard(void *ctx, const char *data, size_t length) {
    (void)ctx;
    (void)data;
    (void)length;
    return true;
}

static const char *const words[] = {
    "class", "class A {", "class A { pub x: int }", "class A { pub read x: int = 1\n fn f(self) => self.x }",
    "class A : B { }", "pub", "pub read", "get", "self", "init", "fn init(self) { }", "pub get g(self) => 1",
    "const C = 1", "override", "operator", "trait", "extend", "super", "A(1)", "A(x: 1)", "a.x", "a.x = 1",
    "a.x += 1", "a.f()", "a.x[0] = 2", "self.x += 1", "a.with(x: 2)", "a.copy()", "identical(a, a)",
    "let", "const", "if", "else", "while", "and", "or", "true", "false", "nil", "x", "y", "z",
    "print", "println", "1", "2", "0", "3.5", "\"s\"", "\"a\\n\"", "+", "-", "*", "/", "//", "%",
    "==", "!=", "&", "^", "~", "<<", ">>", "x & 3 | 4 ^ ~z << 1 >> 2", "<", "<=", ">", ">=", "!", "=", "(", ")", "{", "}", ",", ";", "\n", "\n",
    "#c\n", "@", "12abc", "\"open", "x = x + 1", "let x = 1", "const K = 2", "{ let y = x }",
    "if x { }", "while x < 3 { x = x + 1 }", ":", "|", "?", "int", "string", "any", "type",
    "break", "continue", "while x < 3 { break }", "while x < 3 { continue }",
    "while true { if x { break } else { continue } }", "fn f() { break }", "breaker", "continued",
    "type T = int | nil", "let x: int? = nil", "let y: \"a\" | 1 = 1", "let z: T = nil",
    "x = \"s\"", "true", "-1", "never", "number", "(int | string)?",
    "[", "]", "[1, 2]", "x[0]", "x[0] = 1", "x[1:]", "x[:2]", "let x: int[] = []", "push", "len",
    "pop", "sort", "range(3)", "insert", "remove", "x + y", "[[]]", "let y: int[][] = [[1]]",
    "for", "in", "by", "..", "..=", "for x in [1, 2] { }", "for i in 0..3 { }",
    "for i, x in x { }", "for x: int in x { }", "for i in 0..=2 by -1 { }",
    "for let i = 0; i < 3; i += 1 { }", "for ;; { break }", "for let i = 0, j = 2; i < j; i += 1, j -= 1 { }",
    "for x in x { x = 1 }", "0..3", "[1, 2]..3",
    "try { } catch e { }", "try { x } finally { y }", "try { } catch RangeError as e { } finally { }",
    "try { } catch Error as _ { } catch { }", "try { return 1 } finally { return 2 }", "throw x", "throw",
    "try { throw RuntimeError(\"m\") } catch e { println(e.message) }", "e.message = 1",
    "for i in 0..3 { try { break } finally { continue } }", "try { } catch RangeError { }",
    "try { } catch RangeError as", "try {", "catch", "finally", "throw RuntimeError(", "x.message",
    "error A", "error A : IOError", "error A : B", "error A : A", "error A :", "error", "error 1",
    "error A : IOError : Error", "error A\nerror A", "fn f() { error A }", "{ error A : IOError }",
    "for i in 0..2 { error A }", "let error = 1", "error = 2", "error + 1", "let x = error", "x error y",
    "error A\nthrow A(\"m\", cause: A(\"c\"), data: [1])", "throw A", "throw IOError",
    "try { } catch A as e { }\nerror A : IOError", "A(\"m\", cause: 1)", "A(\"m\", cause:", "A(\"m\", data:",
    "IOError(\"m\", cause: IOError(\"c\"), data: {})", "IOError(\"m\", color: 1)", "e.cause", "e.data.x",
    "error A\nlet d = {}\nd[\"e\"] = A(\"m\", data: d)", "error X : Y\nerror Y : X", "let A = 1\nerror A",
    "switch x { case 1 { } else { } }", "switch { case x { } }", "switch x { case 1 => 2 else => 3 }",
    "let a = switch x { case 1, 2 => y case 3..=9 => z else => w }", "switch x { case 1..3 by 2 { } }",
    "switch x { else { } case 1 { } }", "switch x { case 1 { } case 1 { } }", "switch x { case", "switch",
    "switch x { case 1 { break } }", "for i in 0..3 { switch i { case 1 { continue } } }",
    "$\"{switch x { case 1 => \\\"a\\\" else => \\\"b\\\" }}\"", "case", "x in y", "x not in y", "x not  in y", "1 in 0..3", "1 not in 0..=3", "x in 0..3 by 2", "x in y..z",
    "for x in x in y { }", "for x in (x in y) { }", "for x in f(x in y) { }", "x ?? y", "x ?? y ?? 0",
    "x or y ?? z", "x ??", "?? x", "let a: int?? = nil", "$\"{x in y}\"", "$\"{x ?? \\\"s\\\"}\"", "not in",
    "if x { 1 } else { 2 }", "let v = if x { 1 } else { 2 }", "if x { 1 }", "if x { let } else { }",
    "if x { 1 } else if y { 2 } else { 3 }", "$\"{if x { 1 } else { 2 }}\"", "if x { 1 } else { while }",
    "dict", "dict[int, string]", "dict", "dict[string]", "dict[,]", "let d: dict = {}",
    "let d: dict[string, int] = {}", "let d: dikt = {}", "{\"a\": 1}", "{}", "{1: 2, true: 3}",
    "{\"a\": 1,}", "{\"a\" 1}", "{\"a\": }", "{\"a\": 1", "{\"a\": 1, \"a\": 2}", "{x: y}",
    "key", "{1.5: 1}", "{[]: 1}", "{{}: 1}", "d[\"a\"]", "d[\"a\"] = 1", "d[1] += 2", "d.k", "d.len()",
    "d.copy()", "d.merge(d)", "for k in d { }", "for k, v in d { }", "fn() => {\"a\": 1}",
    "fn() => { }", "$\"{ {\"a\": 1}[\"a\"] }\"",
};

static void run_one(void) {
    char text[512];
    size_t length = 0;
    uint32_t count = below(40);
    for (uint32_t i = 0; i < count; i++) {
        const char *word = words[below(sizeof words / sizeof words[0])];
        size_t size = strlen(word);
        if (length + size + 2 >= sizeof text) {
            break;
        }
        memcpy(text + length, word, size);
        length += size;
        if (below(3) != 0) {
            text[length++] = ' ';
        }
    }
    text[length] = '\0';

    LzWriter sink = {discard, NULL, NULL};
    LzVM vm;
    lz_vm_init(&vm, sink, sink);
    LZ_CHECK(lz_builtin_install(&vm) == LZ_OK);
    lz_vm_set_heap_limit(&vm, 1u << 20);

    LzSource source;
    lz_source_from_buffer(&source, "fuzz.lz", text, length);
    LzChunk chunk;
    lz_chunk_init(&chunk);
    LzDiagnostics diagnostics;

    LzStatus status = lz_compile(&vm, &source, &chunk, &diagnostics);
    LZ_CHECK(status == LZ_OK || status == LZ_ERR_COMPILE || status == LZ_ERR_HEAP_LIMIT);
    LZ_CHECK(diagnostics.count <= LZ_MAX_DIAGNOSTICS);
    LZ_CHECK((status == LZ_ERR_COMPILE) == (diagnostics.count > 0));
    for (size_t i = 0; i < diagnostics.count; i++) {
        LZ_CHECK((size_t)diagnostics.items[i].offset <= length);
        LZ_CHECK((size_t)diagnostics.items[i].offset + diagnostics.items[i].length <= length);
    }

    if (status == LZ_OK) {
        lz_vm_set_fuel(&vm, 20000);
        LzStatus ran = lz_vm_run(&vm, &chunk);
        LZ_CHECK(ran == LZ_OK || ran == LZ_ERR_RUNTIME || ran == LZ_ERR_ARITY ||
                 ran == LZ_ERR_FUEL || ran == LZ_ERR_HEAP_LIMIT || ran == LZ_ERR_STACK_OVERFLOW ||
                 ran == LZ_ERR_TYPE);
    }
    LZ_CHECK(vm.stack_count == 0);

    lz_chunk_free(&vm.heap, &chunk);
    lz_vm_release_roots(&vm);
    LZ_CHECK(vm.heap.live_objects == 0);
    lz_vm_free(&vm);
}

int main(int argc, char **argv) {
    unsigned long iterations = argc > 1 ? strtoul(argv[1], NULL, 10) : 5000;
    for (unsigned long i = 0; i < iterations && lz_test_failures == 0; i++) {
        run_one();
    }
    if (lz_test_failures != 0) {
        fprintf(stderr, "fuzz_compile failed\n");
    }
    return LZ_TEST_RESULT();
}
