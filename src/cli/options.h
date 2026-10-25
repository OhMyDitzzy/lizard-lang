#ifndef LIZARD_CLI_OPTIONS_H
#define LIZARD_CLI_OPTIONS_H

#include <stdbool.h>
#include <stdint.h>

typedef struct LzOptions {
    bool show_help;
    bool show_version;
    bool heap_stats;
    bool trace_memory;
    bool dump_tokens;
    bool has_max_heap;
    uint64_t max_heap_bytes;
    bool has_fuel;
    uint64_t fuel;
    bool has_engine;
    char engine[16]; /* "ref", "reference", "ge" or "gecko"; the default engine when not given */
    bool engine_stats;
    bool has_jit;
    char jit[8]; /* "on", "off" or "eager"; the engine decides when not given */
    bool has_skink;
    bool skink; /* --skink=on or off; the cycle collector is on when not given */
    const char *script_path;
    int script_argc;
    char **script_argv;
    char error[128];
} LzOptions;

/* Options end at the first non-option argument; everything after it belongs to the script. */
bool lz_options_parse(LzOptions *options, int argc, char **argv);

#endif
