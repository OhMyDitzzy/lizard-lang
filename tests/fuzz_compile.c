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
    "let", "const", "if", "else", "while", "and", "or", "true", "false", "nil", "x", "y", "z",
    "print", "println", "1", "2", "0", "3.5", "\"s\"", "\"a\\n\"", "+", "-", "*", "/", "//", "%",
    "==", "!=", "<", "<=", ">", ">=", "!", "=", "(", ")", "{", "}", ",", ";", "\n", "\n",
    "#c\n", "@", "12abc", "\"open", "x = x + 1", "let x = 1", "const K = 2", "{ let y = x }",
    "if x { }", "while x < 3 { x = x + 1 }", ":", "|", "?", "int", "string", "any", "type",
    "type T = int | nil", "let x: int? = nil", "let y: \"a\" | 1 = 1", "let z: T = nil",
    "x = \"s\"", "true", "-1", "never", "number", "(int | string)?",
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
