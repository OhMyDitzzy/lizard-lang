#ifndef LIZARD_UTILS_ARGPARSE_H
#define LIZARD_UTILS_ARGPARSE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ANSI color codes for terminal output */
#define LZ_COLOR_RESET   "\033[0m"
#define LZ_COLOR_BOLD    "\033[1m"
#define LZ_COLOR_DIM     "\033[2m"
#define LZ_COLOR_RED     "\033[31m"
#define LZ_COLOR_GREEN   "\033[32m"
#define LZ_COLOR_YELLOW  "\033[33m"
#define LZ_COLOR_BLUE    "\033[34m"
#define LZ_COLOR_MAGENTA "\033[35m"
#define LZ_COLOR_CYAN    "\033[36m"
#define LZ_COLOR_WHITE   "\033[37m"
#define LZ_COLOR_GRAY    "\033[90m"

/* Argument types */
typedef enum {
    LZ_ARG_FLAG,        /* Boolean flag: --verbose, -v */
    LZ_ARG_OPTION,      /* Option with value: --output=file, -o file */
    LZ_ARG_POSITIONAL,  /* Positional argument: <file> */
    LZ_ARG_HELP,        /* Built-in help flag: --help, -h */
    LZ_ARG_VERSION      /* Built-in version flag: --version, -V */
} LzArgType;

/* Argument definition */
typedef struct LzArg {
    const char *short_name;    /* Short name (single char, e.g., 'h') */
    const char *long_name;     /* Long name (e.g., "help") */
    LzArgType type;            /* Argument type */
    const char *description;   /* Help description */
    const char *metavar;       /* Metavar for help (e.g., "FILE", "N") */
    const char *default_value; /* Default value for options */
    bool required;             /* Whether this argument is required */
    void *dest;                /* Destination pointer to store parsed value */
    const char **choices;      /* Array of valid choices (NULL-terminated) */
    size_t num_choices;        /* Number of choices */
} LzArg;

/* Parse result */
typedef struct LzArgParseResult {
    bool success;
    const char *error_message;
    char error_buffer[512];
    int positional_count;
    const char **positional_args;
} LzArgParseResult;

/* Argument parser context */
typedef struct LzArgParser {
    const char *program_name;
    const char *description;
    const char *epilog;
    const char *version;
    const LzArg *args;
    size_t num_args;
    bool color_enabled;
    bool allow_unknown;
    const char *error_prefix;
} LzArgParser;

/* Initialize argument parser */
void lz_argparse_init(LzArgParser *parser, const char *program_name);

/* Set parser properties */
void lz_argparse_set_description(LzArgParser *parser, const char *description);
void lz_argparse_set_epilog(LzArgParser *parser, const char *epilog);
void lz_argparse_set_version(LzArgParser *parser, const char *version);
void lz_argparse_set_args(LzArgParser *parser, const LzArg *args, size_t num_args);
void lz_argparse_enable_color(LzArgParser *parser, bool enable);
void lz_argparse_allow_unknown(LzArgParser *parser, bool allow);

/* Parse command line arguments */
LzArgParseResult lz_argparse_parse(LzArgParser *parser, int argc, char **argv);

/* Print help message */
void lz_argparse_print_help(const LzArgParser *parser, FILE *stream);

/* Print version message */
void lz_argparse_print_version(const LzArgParser *parser, FILE *stream);

/* Free parse result resources */
void lz_argparse_free_result(LzArgParseResult *result);

/* Helper macros for defining arguments */
#define LZ_ARG_FLAG(s_, l_, desc_, dest_) \
    { .short_name = s_, .long_name = l_, .type = LZ_ARG_FLAG, .description = desc_, .dest = dest_ }

#define LZ_ARG_OPTION(s_, l_, desc_, metavar_, dest_, def_) \
    { .short_name = s_, .long_name = l_, .type = LZ_ARG_OPTION, .description = desc_, .metavar = metavar_, .dest = dest_, .default_value = def_ }

#define LZ_ARG_POSITIONAL(l_, desc_, metavar_, dest_, req_) \
    { .short_name = NULL, .long_name = l_, .type = LZ_ARG_POSITIONAL, .description = desc_, .metavar = metavar_, .dest = dest_, .required = req_ }

#define LZ_ARG_HELP(s_, l_, dest_) \
    { .short_name = s_, .long_name = l_, .type = LZ_ARG_HELP, .dest = dest_ }

#define LZ_ARG_VERSION(s_, l_, dest_) \
    { .short_name = s_, .long_name = l_, .type = LZ_ARG_VERSION, .dest = dest_ }

#define LZ_ARG_END { 0 }

/* Parse a count string with optional K/M/G suffix (powers of 1024) */
bool lz_parse_count(const char *text, bool allow_suffix, uint64_t *out);

#ifdef __cplusplus
}
#endif

#endif /* LIZARD_UTILS_ARGPARSE_H */