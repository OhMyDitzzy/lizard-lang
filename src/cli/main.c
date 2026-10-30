#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L /* fileno, isatty */
#endif

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "lizard/builtin.h"
#include "lizard/compiler.h"
#include "lizard/engine.h"
#include "lizard/lexer.h"
#include "lizard/report.h"
#include "lizard/vm.h"
#include "lizard/version.h"
#include "lizard/utils/argparse.h"

#ifdef _WIN32
#include <io.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

enum {
    EXIT_USAGE = 64,
    EXIT_DATA_ERROR = 65,
    EXIT_NO_INPUT = 66,
    EXIT_RUNTIME_ERROR = 70
};

/* Colour for error reports: --color=auto|always|never, NO_COLOR, FORCE_COLOR. */
static bool stderr_is_terminal(void) {
#ifdef _WIN32
    if (!_isatty(_fileno(stderr))) {
        return false;
    }
    HANDLE handle = GetStdHandle(STD_ERROR_HANDLE);
    DWORD mode = 0;
    if (handle == INVALID_HANDLE_VALUE || !GetConsoleMode(handle, &mode)) {
        return false;
    }
    /* Older consoles print the escape codes as text unless virtual terminal output is on. */
    return SetConsoleMode(handle, mode | 0x0004 /* ENABLE_VIRTUAL_TERMINAL_PROCESSING */) != 0;
#else
    const char *term = getenv("TERM");
    return isatty(fileno(stderr)) && (term == NULL || strcmp(term, "dumb") != 0);
#endif
}

static bool env_set(const char *name) {
    const char *value = getenv(name);
    return value != NULL && value[0] != '\0';
}

static bool decide_color(const char *mode) {
    if (mode != NULL && strcmp(mode, "always") == 0) {
        return true;
    }
    if (mode != NULL && strcmp(mode, "never") == 0) {
        return false;
    }
    if (env_set("NO_COLOR")) {
        return false;
    }
    if (env_set("FORCE_COLOR") || env_set("CLICOLOR_FORCE")) {
        return true;
    }
    return stderr_is_terminal();
}

/* "lizard: error: message" for failures that have no place in the source. */
static void report_plain_error(bool color, const char *message) {
    fprintf(stderr, "%slizard: error%s: %s\n", color ? "\x1b[1;31m" : "", color ? "\x1b[0m" : "",
            message);
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
    if (stats->suspects == 0 && stats->watched == 0 && stats->full_passes == 0) {
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
    if (stats->full_passes > 0) {
        fprintf(stderr, "skink: %llu full passes (%llu before --max-heap refused an allocation), %llu bytes freed\n",
                (unsigned long long)stats->full_passes, (unsigned long long)stats->reclaims,
                (unsigned long long)stats->reclaimed);
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

static int dump_tokens(const char *path, bool color) {
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
            LzReport report = {0};
            report.severity = "error";
            report.message = token.value.error;
            report.offset = token.offset;
            report.length = token.length;
            lz_report_write(&source, &report, color, &err);
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

static void report_runtime_error(const LzVM *vm, const LzSource *source,
                                 const LzDiagnostics *diagnostics, bool color) {
    if (vm->error_offset == LZ_NO_OFFSET) {
        report_plain_error(color, vm->error);
        return;
    }

    LzWriter err = lz_writer_from_file(stderr);
    LzReport report = {0};
    report.severity = "error";
    report.message = vm->error;
    report.offset = vm->error_offset;
    report.length = 0;

    /* An undefined name: the compiler kept what it knew about the names in scope. */
    char headline[160];
    const LzNameHint *hint = lz_diagnostics_find_hint(diagnostics, vm->error_offset);
    if (hint != NULL && strncmp(vm->error, "undefined variable '", 20) == 0) {
        report.length = hint->length;
        report.label = "not found in this scope";
        report.suggestions = &hint->suggestions;
        if (hint->is_method) {
            (void)snprintf(headline, sizeof headline, "undefined method%s", vm->error + 18);
            report.message = headline;
            report.label = "no function or method with this name";
        } else if (hint->is_call) {
            (void)snprintf(headline, sizeof headline, "undefined function%s", vm->error + 18);
            report.message = headline;
        }
    }

    LzReportFrame frames[LZ_TRACE_MAX];
    char names[LZ_TRACE_MAX][48];
    for (size_t i = 0; i < vm->trace_count && i < LZ_TRACE_MAX; i++) {
        const LzTraceEntry *entry = &vm->trace[i];
        (void)snprintf(names[i], sizeof names[i], "%s", entry->name);
        frames[i].name = names[i];
        frames[i].has_location = entry->offset != LZ_NO_OFFSET;
        frames[i].line = 0;
        frames[i].column = 0;
        if (frames[i].has_location) {
            LzLocation where = lz_source_locate(source, entry->offset);
            frames[i].line = where.line;
            frames[i].column = where.column;
        }
    }
    if (vm->trace_count > 0) {
        report.frames = frames;
        report.frame_count = vm->trace_count < LZ_TRACE_MAX ? vm->trace_count : LZ_TRACE_MAX;
        if (vm->trace_total > vm->trace_count) {
            report.frames_omitted = vm->trace_total - vm->trace_count;
        }
    }

    lz_report_write(source, &report, color, &err);
}

static int run_script(const char *script_path, bool heap_stats, bool trace_memory,
                      bool has_max_heap, uint64_t max_heap_bytes,
                      bool has_fuel, uint64_t fuel, bool has_engine, const char *engine,
                      bool engine_stats, bool has_jit, const char *jit,
                      bool has_skink, bool skink, bool color) {
    LzSource source;
    char error[200];
    if (!lz_source_read_file(&source, script_path, error, sizeof error)) {
        fprintf(stderr, "lizard: %s\n", error);
        return EXIT_NO_INPUT;
    }

    LzVM vm;
    lz_vm_init(&vm, lz_writer_from_file(stdout), lz_writer_from_file(stderr));
    vm.heap.trace = trace_memory;
    if (has_skink) {
        vm.skink.enabled = skink;
    }
    /* The final sweep only makes the report of what is left alive exact: skip it when nobody asked. */
    vm.skink.final_sweep = heap_stats || trace_memory;

    LzChunk chunk;
    lz_chunk_init(&chunk);
    int exit_code = EXIT_SUCCESS;

    if (lz_builtin_install(&vm) != LZ_OK) {
        fprintf(stderr, "lizard: %s\n", vm.error);
        exit_code = EXIT_RUNTIME_ERROR;
    } else {
        if (has_max_heap) {
            lz_vm_set_heap_limit(&vm, (size_t)max_heap_bytes);
        }
        if (has_fuel) {
            lz_vm_set_fuel(&vm, fuel);
        }

        LzEngine eng = LZ_ENGINE_GE;
        if (has_engine) {
            (void)lz_engine_from_name(engine, &eng); /* validated while parsing */
        }
        if (has_jit) {
            LzJitMode mode = LZ_JIT_ON;
            (void)lz_jit_mode_from_name(jit, &mode); /* validated while parsing */
            lz_engine_set_jit(mode);
        }
        LzEngineStats stats = {0};

        LzDiagnostics diagnostics;
        LzStatus status = lz_compile(&vm, &source, &chunk, &diagnostics);
        if (status == LZ_ERR_COMPILE) {
            lz_writer_flush(&vm.out);
            lz_diagnostics_report(&diagnostics, &source, color, &vm.err);
            exit_code = EXIT_DATA_ERROR;
        } else if (status != LZ_OK) {
            report_plain_error(color, vm.error);
            exit_code = EXIT_RUNTIME_ERROR;
        } else {
            status = lz_engine_run(eng, &vm, &chunk, engine_stats ? &stats : NULL);
            if (status != LZ_OK) {
                lz_writer_flush(&vm.out);
                report_runtime_error(&vm, &source, &diagnostics, color);
                exit_code = EXIT_RUNTIME_ERROR;
            }
            if (engine_stats) {
                print_engine_stats(eng, &stats);
            }
        }
    }

    lz_chunk_free(&vm.heap, &chunk);
    lz_vm_release_roots(&vm);
    if (heap_stats) {
        print_heap_stats(&vm.heap);
        print_skink_stats(&vm.skink.stats);
    }
    lz_vm_free(&vm);
    lz_source_free(&source);
    return exit_code;
}

int main(int argc, char **argv) {
    bool show_help = false;
    bool show_version = false;
    bool heap_stats = false;
    bool trace_memory = false;
    bool dump_tokens_flag = false;
    bool engine_stats = false;
    bool has_max_heap = false;
    uint64_t max_heap_bytes = 0;
    const char *max_heap_str = NULL;
    bool has_fuel = false;
    uint64_t fuel = 0;
    const char *fuel_str = NULL;
    bool has_engine = false;
    const char *engine = NULL;
    bool has_jit = false;
    const char *jit = NULL;
    bool has_skink = false;
    bool skink = false;
    const char *skink_str = NULL;
    const char *color_str = NULL;
    const char *script_path = NULL;

    const char *engine_choices[] = {"ge", "gecko", "ref", "reference", NULL};
    const char *jit_choices[] = {"on", "off", "eager", NULL};
    const char *skink_choices[] = {"on", "off", NULL};
    const char *color_choices[] = {"auto", "always", "never", NULL};

    LzArg args[] = {
        LZ_ARG_HELP("h", "help", &show_help),
        LZ_ARG_VERSION("V", "version", &show_version),
        LZ_ARG_FLAG(NULL, "heap-stats", "Print heap statistics when the program ends", &heap_stats),
        LZ_ARG_FLAG(NULL, "trace-memory", "Log every heap allocation and release to stderr", &trace_memory),
        LZ_ARG_FLAG(NULL, "dump-tokens", "Print the tokens of the script and exit", &dump_tokens_flag),
        LZ_ARG_FLAG(NULL, "engine-stats", "Print what the engine did when the program ends", &engine_stats),
        LZ_ARG_OPTION(NULL, "max-heap", "Limit live heap bytes (suffix K, M or G allowed)", "N", &max_heap_str, NULL),
        LZ_ARG_OPTION(NULL, "fuel", "Limit the number of executed instructions", "N", &fuel_str, NULL),
        LZ_ARG_OPTION(NULL, "engine", "Execution engine: ge (default, the fast one) or ref", "E", &engine, NULL),
        LZ_ARG_OPTION(NULL, "jit", "Machine code for hot code: on (default), off or eager", "M", &jit, NULL),
        LZ_ARG_OPTION(NULL, "skink", "Cycle collector for closures: on (default) or off", "M", &skink_str, NULL),
        LZ_ARG_OPTION(NULL, "color", "Colour in error messages: auto (default), always or never", "WHEN", &color_str, NULL),
        LZ_ARG_POSITIONAL("script", "Lizard script file to execute", "FILE", &script_path, true),
        LZ_ARG_END
    };

    /* Set choices for validation */
    args[8].choices = engine_choices;
    args[8].num_choices = 4;
    args[9].choices = jit_choices;
    args[9].num_choices = 3;
    args[10].choices = skink_choices;
    args[10].num_choices = 2;
    args[11].choices = color_choices;
    args[11].num_choices = 3;

    LzArgParser parser;
    lz_argparse_init(&parser, "lizard");
    // TODO: Just put the website domain, we can built the web later
    lz_argparse_set_description(&parser, "Lizard is a General Purpose programming language which is multi-functional and designed to be easy to understand. See more information at: lizardlang.org");
    lz_argparse_set_version(&parser, LZ_VERSION_LONG);
    lz_argparse_set_args(&parser, args, sizeof(args) / sizeof(args[0]) - 1);
    lz_argparse_enable_color(&parser, true);

    LzArgParseResult result = lz_argparse_parse(&parser, argc, argv);

    if (show_help) {
        return EXIT_SUCCESS;
    }
    if (show_version) {
        /* Version is already printed by the parser */
        return EXIT_SUCCESS;
    }

    if (!result.success) {
        fprintf(stderr, "lizard: %s\nTry 'lizard --help'.\n", result.error_message);
        lz_argparse_free_result(&result);
        return EXIT_USAGE;
    }

    /* Check for --max-heap value */
    if (max_heap_str) {
        if (!lz_parse_count(max_heap_str, true, &max_heap_bytes) || max_heap_bytes > SIZE_MAX) {
            fprintf(stderr, "lizard: invalid value for --max-heap: '%s'\n", max_heap_str);
            lz_argparse_free_result(&result);
            return EXIT_USAGE;
        }
        has_max_heap = true;
    }

    /* Check for --fuel value */
    if (fuel_str) {
        if (!lz_parse_count(fuel_str, false, &fuel)) {
            fprintf(stderr, "lizard: invalid value for --fuel: '%s'\n", fuel_str);
            lz_argparse_free_result(&result);
            return EXIT_USAGE;
        }
        has_fuel = true;
    }

    /* Check if --engine was explicitly provided */
    has_engine = (engine != NULL);
    if (has_engine && strcmp(engine, "ge") == 0) {
        /* If user explicitly passed --engine=ge, we still respect it */
    }

    /* Check if --jit was explicitly provided */
    has_jit = (jit != NULL);
    if (has_jit && strcmp(jit, "on") == 0) {
        /* If user explicitly passed --jit=on, we still respect it */
    }

    /* Check if --skink was explicitly provided */
    has_skink = (skink_str != NULL);
    if (has_skink) {
        skink = (strcmp(skink_str, "on") == 0);
    }

    if (dump_tokens_flag) {
        lz_argparse_free_result(&result);
        return dump_tokens(script_path, decide_color(color_str));
    }

    if (!has_lz_extension(script_path)) {
        fprintf(stderr, "lizard: warning: '%s' does not have the .lz extension\n",
                script_path);
    }

    lz_argparse_free_result(&result);
    return run_script(script_path, heap_stats, trace_memory,
                      has_max_heap, max_heap_bytes, has_fuel, fuel,
                      has_engine, engine, engine_stats, has_jit, jit,
                      has_skink, skink, decide_color(color_str));
}