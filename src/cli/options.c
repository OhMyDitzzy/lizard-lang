#include "options.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static bool is_flag(const char *arg, const char *short_name, const char *long_name) {
    return strcmp(arg, short_name) == 0 || strcmp(arg, long_name) == 0;
}

static const char *option_value(const char *arg, const char *name) {
    size_t length = strlen(name);
    if (strncmp(arg, name, length) == 0 && arg[length] == '=') {
        return arg + length + 1;
    }
    return NULL;
}

static const char *const engine_names[] = {"ref", "reference", "ge", "gecko"};

static bool is_engine_name(const char *text) {
    for (size_t i = 0; i < sizeof engine_names / sizeof engine_names[0]; i++) {
        if (strcmp(text, engine_names[i]) == 0) {
            return true;
        }
    }
    return false;
}

/* Plain decimal digits, optionally followed by one K, M or G (powers of 1024). */
static bool parse_count(const char *text, bool allow_suffix, uint64_t *out) {
    if (*text < '0' || *text > '9') {
        return false;
    }

    uint64_t value = 0;
    for (; *text >= '0' && *text <= '9'; text++) {
        uint64_t digit = (uint64_t)(*text - '0');
        if (value > (UINT64_MAX - digit) / 10) {
            return false;
        }
        value = value * 10 + digit;
    }

    if (*text != '\0') {
        if (!allow_suffix || text[1] != '\0') {
            return false;
        }
        unsigned shift;
        switch (*text) {
        case 'k':
        case 'K':
            shift = 10;
            break;
        case 'm':
        case 'M':
            shift = 20;
            break;
        case 'g':
        case 'G':
            shift = 30;
            break;
        default:
            return false;
        }
        if (value > (UINT64_MAX >> shift)) {
            return false;
        }
        value <<= shift;
    }

    *out = value;
    return true;
}

bool lz_options_parse(LzOptions *options, int argc, char **argv) {
    memset(options, 0, sizeof *options);

    int i = 1;
    for (; i < argc; i++) {
        const char *arg = argv[i];

        if (strcmp(arg, "--") == 0) {
            i++;
            break;
        }
        if (arg[0] != '-' || arg[1] == '\0') {
            break;
        }

        if (is_flag(arg, "-h", "--help")) {
            options->show_help = true;
        } else if (is_flag(arg, "-V", "--version")) {
            options->show_version = true;
        } else if (strcmp(arg, "--heap-stats") == 0) {
            options->heap_stats = true;
        } else if (strcmp(arg, "--trace-memory") == 0) {
            options->trace_memory = true;
        } else if (strcmp(arg, "--dump-tokens") == 0) {
            options->dump_tokens = true;
        } else if (strcmp(arg, "--engine-stats") == 0) {
            options->engine_stats = true;
        } else if (option_value(arg, "--engine") != NULL) {
            const char *name = option_value(arg, "--engine");
            if (!is_engine_name(name)) {
                (void)snprintf(options->error, sizeof options->error,
                               "invalid value for --engine: '%.40s' (expected ge or ref)", name);
                return false;
            }
            options->has_engine = true;
            (void)snprintf(options->engine, sizeof options->engine, "%s", name);
        } else if (option_value(arg, "--max-heap") != NULL) {
            uint64_t bytes;
            if (!parse_count(option_value(arg, "--max-heap"), true, &bytes) || bytes > SIZE_MAX) {
                (void)snprintf(options->error, sizeof options->error,
                               "invalid value for --max-heap: '%s'",
                               option_value(arg, "--max-heap"));
                return false;
            }
            options->has_max_heap = true;
            options->max_heap_bytes = bytes;
        } else if (option_value(arg, "--fuel") != NULL) {
            if (!parse_count(option_value(arg, "--fuel"), false, &options->fuel)) {
                (void)snprintf(options->error, sizeof options->error,
                               "invalid value for --fuel: '%s'", option_value(arg, "--fuel"));
                return false;
            }
            options->has_fuel = true;
        } else {
            (void)snprintf(options->error, sizeof options->error, "unknown option '%s'", arg);
            return false;
        }
    }

    if (i < argc) {
        options->script_path = argv[i];
        options->script_argc = argc - i;
        options->script_argv = &argv[i];
    }
    return true;
}
