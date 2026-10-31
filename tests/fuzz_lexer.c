#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lizard/lexer.h"
#include "test_util.h"

static uint64_t state = 0xA24BAED4963EE407ull;

static uint64_t next_random(void) {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return state;
}

static uint32_t below(uint32_t limit) {
    return (uint32_t)(next_random() % limit);
}

static const char alphabet[] = "abz_019 .eExXbBoO+-*/%()[]{}<>=!,:;#\"'\\\n\r\t@{}uU";

static void run_one(void) {
    char text[160];
    size_t length = below(sizeof text);
    for (size_t i = 0; i < length; i++) {
        text[i] = below(4) == 0 ? (char)below(256) : alphabet[below(sizeof alphabet - 1)];
    }

    LzSource source;
    lz_source_from_buffer(&source, "fuzz.lz", text, length);
    LzLexer lexer;
    lz_lexer_init(&lexer, &source);

    size_t previous_end = 0;
    size_t calls = 0;
    for (;;) {
        LzToken token = lz_lexer_next(&lexer);
        calls++;
        LZ_CHECK(calls <= 2 * length + 4);
        LZ_CHECK((size_t)token.offset + token.length <= length);
        LZ_CHECK(token.offset >= previous_end || token.type == LZ_TOKEN_EOF);
        previous_end = (size_t)token.offset + token.length;

        if (token.type == LZ_TOKEN_STRING && !token.value.string.is_template) {
            LzBuffer value = {0};
            LZ_CHECK(lz_token_string_value(&source, &token, &value));
            LZ_CHECK(value.length <= token.length * 4);
            lz_buffer_free(&value);
        }
        if (token.type == LZ_TOKEN_ERROR) {
            LZ_CHECK(token.value.error != NULL && token.length > 0);
        }
        if (token.type == LZ_TOKEN_EOF || lz_test_failures != 0) {
            break;
        }
    }

    LzToken after = lz_lexer_next(&lexer);
    LZ_CHECK(after.type == LZ_TOKEN_EOF);
}

int main(int argc, char **argv) {
    unsigned long iterations = argc > 1 ? strtoul(argv[1], NULL, 10) : 20000;
    for (unsigned long i = 0; i < iterations && lz_test_failures == 0; i++) {
        run_one();
    }
    return LZ_TEST_RESULT();
}
