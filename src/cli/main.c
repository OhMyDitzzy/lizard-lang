#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "lizard/builtin.h"
#include "lizard/compiler.h"
#include "lizard/engine.h"
#include "lizard/lexer.h"
#include "lizard/vm.h"
#include "lizard/version.h"
#include "options.h"

enum {
    EXIT_USAGE = 64,
    EXIT_DATA_ERROR = 65,
    EXIT_NO_INPUT = 66,
    EXIT_RUNTIME_ERROR = 70
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
          "      --engine=E   Execution engine: ge (default, the fast one) or ref\n"
          "      --engine-stats  Print what the engine did when the program ends\n"
          "      --jit=M      Machine code for hot code: on (default), off or eager\n"
          "      --skink=M    Cycle collector for closures: on (default) or off\n"
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

static void print_skink_stats(const LzSkinkStats *stats) {
    if (stats->suspects == 0 && stats->watched == 0 && stats->reclaims == 0) {
        return;
    }
    fprintf(stderr,
            "skink: %llu suspects (%llu watched, %llu of them by assignment), %llu trials (%llu "
            "from watch lists, %llu over budget), %llu dead cycles freed (%llu objects), "
            "%llu ticks from allocations\n",
            (unsigned long long)stats->suspects, (unsigned long long)stats->watched,
            (unsigned long long)stats->stored,
            (unsigned long long)stats->trials, (unsigned long long)stats->polls,
            (unsigned long long)stats->over_budget, (unsigned long long)stats->cycles,
            (unsigned long long)stats->objects, (unsigned long long)stats->alloc_ticks);
    if (stats->reclaims > 0) {
        fprintf(stderr, "skink: %llu emergency passes before --max-heap refused an allocation, %llu bytes freed\n",
                (unsigned long long)stats->reclaims, (unsigned long long)stats->reclaimed);
    }
}

static void print_engine_stats(LzEngine engine, const LzEngineStats *stats) {
    fprintf(stderr, "engine %s: %llu instructions", lz_engine_name(engine),
            (unsigned long long)stats->instructions);
    if (engine == LZ_ENGINE_GE && stats->translated) {
        double ratio = stats->dispatches > 0 ? (double)stats->instructions / (double)stats->dispatches : 1.0;
        fprintf(stderr, ", %llu dispatches (%.2fx fewer), %llu delegated, %zu fused sites in %zu ops",
                (unsigned long long)stats->dispatches, ratio, (unsigned long long)stats->delegated,
                stats->fused_sites, stats->program_size);
        if (stats->jit_programs > 0) {
            fprintf(stderr, ", jit: %zu programs, %zu bytes, %llu entries", stats->jit_programs,
                    stats->jit_bytes, (unsigned long long)stats->jit_enters);
        }
    } else if (engine == LZ_ENGINE_GE) {
        fputs(" (chunk not translatable: ran on the reference interpreter)", stderr);
    }
    fputc('\n', stderr);
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
    return failed ? EXIT_DATA_ERROR : EXIT_SUCCESS;
}

static void report_runtime_error(const LzVM *vm, const LzSource *source) {
    LzWriter err = lz_writer_from_file(stderr);
    if (vm->error_offset != LZ_NO_OFFSET) {
        lz_source_write_diagnostic(source, vm->error_offset, 0, "error", vm->error, &err);
        if (vm->trace_count > 0) {
            fprintf(stderr, "stack trace (innermost call first):\n");
            for (size_t i = 0; i < vm->trace_count; i++) {
                const LzTraceEntry *entry = &vm->trace[i];
                if (entry->offset == LZ_NO_OFFSET) {
                    fprintf(stderr, "  at %s\n", entry->name);
                } else {
                    LzLocation where = lz_source_locate(source, entry->offset);
                    fprintf(stderr, "  at %s (%s:%u:%u)\n", entry->name, source->name,
                            (unsigned)where.line, (unsigned)where.column);
                }
            }
            if (vm->trace_total > vm->trace_count) {
                fprintf(stderr, "  ... %zu more frames\n", vm->trace_total - vm->trace_count);
            }
        }
    } else {
        fprintf(stderr, "lizard: error: %s\n", vm->error);
    }
}

static int run_script(const LzOptions *options) {
    LzSource source;
    char error[200];
    if (!lz_source_read_file(&source, options->script_path, error, sizeof error)) {
        fprintf(stderr, "lizard: %s\n", error);
        return EXIT_NO_INPUT;
    }

    LzVM vm;
    lz_vm_init(&vm, lz_writer_from_file(stdout), lz_writer_from_file(stderr));
    vm.heap.trace = options->trace_memory;
    if (options->has_skink) {
        vm.skink.enabled = options->skink;
    }

    LzChunk chunk;
    lz_chunk_init(&chunk);
    int exit_code = EXIT_SUCCESS;

    if (lz_builtin_install(&vm) != LZ_OK) {
        fprintf(stderr, "lizard: %s\n", vm.error);
        exit_code = EXIT_RUNTIME_ERROR;
    } else {
        if (options->has_max_heap) {
            lz_vm_set_heap_limit(&vm, (size_t)options->max_heap_bytes);
        }
        if (options->has_fuel) {
            lz_vm_set_fuel(&vm, options->fuel);
        }

        LzEngine engine = LZ_ENGINE_GE;
        if (options->has_engine) {
            (void)lz_engine_from_name(options->engine, &engine); /* validated while parsing */
        }
        if (options->has_jit) {
            LzJitMode mode = LZ_JIT_ON;
            (void)lz_jit_mode_from_name(options->jit, &mode); /* validated while parsing */
            lz_engine_set_jit(mode);
        }
        LzEngineStats stats = {0};

        LzDiagnostics diagnostics;
        LzStatus status = lz_compile(&vm, &source, &chunk, &diagnostics);
        if (status == LZ_ERR_COMPILE) {
            lz_diagnostics_write(&diagnostics, &source, &vm.err);
            exit_code = EXIT_DATA_ERROR;
        } else if (status != LZ_OK) {
            fprintf(stderr, "lizard: error: %s\n", vm.error);
            exit_code = EXIT_RUNTIME_ERROR;
        } else {
            status = lz_engine_run(engine, &vm, &chunk, options->engine_stats ? &stats : NULL);
            if (status != LZ_OK) {
                lz_writer_flush(&vm.out);
                report_runtime_error(&vm, &source);
                exit_code = EXIT_RUNTIME_ERROR;
            }
            if (options->engine_stats) {
                print_engine_stats(engine, &stats);
            }
        }
    }

    lz_chunk_free(&vm.heap, &chunk);
    lz_vm_release_roots(&vm);
    if (options->heap_stats) {
        print_heap_stats(&vm.heap);
        print_skink_stats(&vm.skink.stats);
    }
    lz_vm_free(&vm);
    lz_source_free(&source);
    return exit_code;
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

    return run_script(&options);
}
