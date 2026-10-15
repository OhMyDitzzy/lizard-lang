#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "lizard/builtin.h"
#include "lizard/lexer.h"
#include "lizard/vm.h"
#include "lizard/version.h"
#include "options.h"

enum {
    EXIT_RUNTIME_ERROR = 1,
    EXIT_USAGE = 64,
    EXIT_NO_INPUT = 66
};

static void print_usage(FILE *stream) {
    fputs("Usage: lizard [options] <script.lz> [script arguments]\n"
          "\n"
          "Options:\n"
          "  -h, --help       Show this help and exit\n"
          "  -V, --version    Show the version and exit\n"
          "      --heap-stats Print heap statistics when the program ends\n"
          "      --trace-memory  Log every heap allocation and release to stderr\n"
          "      --dump-tokens Print the tokens of the script and exit\n"
          "      --max-heap=N Limit live heap bytes (suffix K, M or G allowed)\n"
          "      --fuel=N     Limit the number of executed instructions\n"
          "      --           Stop option parsing\n",
          stream);
}

static bool has_lz_extension(const char *path) {
    const char *dot = strrchr(path, '.');
    return dot != NULL && strcmp(dot, ".lz") == 0;
}

static void print_heap_stats(const LzHeap *heap) {
    fprintf(stderr,
            "heap: %zu allocations, peak %zu bytes, %zu objects (%zu bytes) still live at exit\n",
            heap->total_allocations, heap->peak_bytes, heap->live_objects, heap->live_bytes);
}

static int dump_tokens(const char *path) {
    LzSource source;
    char error[200];
    if (!lz_source_read_file(&source, path, error, sizeof error)) {
        fprintf(stderr, "lizard: %s\n", error);
        return EXIT_NO_INPUT;
    }

    LzWriter err = lz_writer_from_file(stderr);
    LzLexer lexer;
    lz_lexer_init(&lexer, &source);

    /* Track line and column incrementally so a large file is not rescanned per token. */
    size_t scanned = 0;
    unsigned line = 1;
    size_t line_start = 0;
    bool failed = false;

    for (;;) {
        LzToken token = lz_lexer_next(&lexer);

        for (; scanned < token.offset; scanned++) {
            if (source.text[scanned] == '\n') {
                line++;
                line_start = scanned + 1;
            }
        }

        if (token.type == LZ_TOKEN_ERROR) {
            failed = true;
            fflush(stdout);
            lz_source_write_diagnostic(&source, token.offset, token.length, "error",
                                       token.value.error, &err);
        } else {
            unsigned column = 1;
            for (size_t i = line_start; i < token.offset; i++) {
                if (((unsigned char)source.text[i] & 0xC0) != 0x80) {
                    column++;
                }
            }
            printf("%u:%u\t%s", line, column, lz_token_type_name(token.type));
            if (token.type == LZ_TOKEN_NEWLINE) {
                printf("\t\\n");
            } else if (token.type != LZ_TOKEN_EOF) {
                printf("\t%.*s", (int)token.length, source.text + token.offset);
            }
            printf("\n");
        }

        if (token.type == LZ_TOKEN_EOF) {
            break;
        }
    }

    lz_source_free(&source);
    return failed ? EXIT_RUNTIME_ERROR : EXIT_SUCCESS;
}

int main(int argc, char **argv) {
    LzOptions options;
    if (!lz_options_parse(&options, argc, argv)) {
        fprintf(stderr, "lizard: %s\nTry 'lizard --help'.\n", options.error);
        return EXIT_USAGE;
    }

    if (options.show_help) {
        print_usage(stdout);
        return EXIT_SUCCESS;
    }
    if (options.show_version) {
        printf("lizard %s\n", LZ_VERSION_STRING);
        return EXIT_SUCCESS;
    }
    if (options.script_path == NULL) {
        print_usage(stderr);
        return EXIT_USAGE;
    }
    if (options.dump_tokens) {
        return dump_tokens(options.script_path);
    }
    if (!has_lz_extension(options.script_path)) {
        fprintf(stderr, "lizard: warning: '%s' does not have the .lz extension\n",
                options.script_path);
    }

    LzVM vm;
    lz_vm_init(&vm, lz_writer_from_file(stdout), lz_writer_from_file(stderr));
    vm.heap.trace = options.trace_memory;

    if (lz_builtin_install(&vm) != LZ_OK) {
        fprintf(stderr, "lizard: %s\n", vm.error);
        lz_vm_free(&vm);
        return EXIT_RUNTIME_ERROR;
    }

    if (options.has_max_heap) {
        lz_vm_set_heap_limit(&vm, (size_t)options.max_heap_bytes);
    }
    if (options.has_fuel) {
        lz_vm_set_fuel(&vm, options.fuel);
    }

    /* The lexer, parser and evaluator are not written yet; syntax is still undecided. */
    lz_writer_write_cstr(&vm.err, "lizard: the language front-end is not implemented yet\n");

    lz_vm_release_roots(&vm);
    if (options.heap_stats) {
        print_heap_stats(&vm.heap);
    }
    lz_vm_free(&vm);
    return EXIT_RUNTIME_ERROR;
}
