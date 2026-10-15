#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "lizard/builtin.h"
#include "lizard/vm.h"
#include "lizard/version.h"
#include "options.h"

enum {
    EXIT_RUNTIME_ERROR = 1,
    EXIT_USAGE = 64
};

static void print_usage(FILE *stream) {
    fputs("Usage: lizard [options] <script.lz> [script arguments]\n"
          "\n"
          "Options:\n"
          "  -h, --help       Show this help and exit\n"
          "  -V, --version    Show the version and exit\n"
          "      --heap-stats Print heap statistics when the program ends\n"
          "      --trace-memory  Log every heap allocation and release to stderr\n"
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
