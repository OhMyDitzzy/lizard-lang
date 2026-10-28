#include "lizard/utils/argparse.h"

#include <stdio.h>
#include <stdbool.h>

int main(int argc, char **argv) {
    bool show_help = false;
    bool show_version = false;
    bool verbose = false;
    bool debug = false;
    const char *output_file = NULL;
    const char *input_file = NULL;
    const char *engine = NULL;
    const char *log_level = NULL;

    LzArg args[] = {
        LZ_ARG_FLAG("v", "verbose", "Enable verbose output", &verbose),
        LZ_ARG_FLAG("d", "debug", "Enable debug mode", &debug),
        LZ_ARG_OPTION("o", "output", "Output file path", "FILE", &output_file, NULL),
        LZ_ARG_OPTION("e", "engine", "Execution engine", "ENGINE", &engine, "ge"),
        LZ_ARG_OPTION("l", "log-level", "Log level", "LEVEL", &log_level, "info"),
        LZ_ARG_POSITIONAL("input", "Input script file", "FILE", &input_file, true),
        LZ_ARG_HELP("h", "help", &show_help),
        LZ_ARG_VERSION("V", "version", &show_version),
        LZ_ARG_END
    };

    LzArgParser parser;
    lz_argparse_init(&parser, "lizard");
    lz_argparse_set_description(&parser, "Lizard - A fast, lightweight scripting language");
    lz_argparse_set_epilog(&parser, "For more information, visit https://github.com/lizard-lang/lizard");
    lz_argparse_set_version(&parser, "lizard 0.1.0");
    lz_argparse_set_args(&parser, args, sizeof(args) / sizeof(args[0]) - 1);
    lz_argparse_enable_color(&parser, true);

    LzArgParseResult result = lz_argparse_parse(&parser, argc, argv);

    if (show_help || show_version) {
        return 0;
    }

    if (!result.success) {
        fprintf(stderr, "%s: %s\n", parser.error_prefix, result.error_message);
        fprintf(stderr, "Try '%s --help' for more information.\n", parser.program_name);
        lz_argparse_free_result(&result);
        return 1;
    }

    printf("Parsed successfully!\n");
    printf("  verbose: %s\n", verbose ? "true" : "false");
    printf("  debug: %s\n", debug ? "true" : "false");
    printf("  output: %s\n", output_file ? output_file : "(none)");
    printf("  engine: %s\n", engine ? engine : "(default)");
    printf("  log-level: %s\n", log_level ? log_level : "(default)");
    printf("  input: %s\n", input_file ? input_file : "(none)");

    if (result.positional_count > 1) {
        printf("  extra positional args:\n");
        for (size_t i = 1; i < (size_t)result.positional_count; i++) {
            printf("    %s\n", result.positional_args[i]);
        }
    }

    lz_argparse_free_result(&result);
    return 0;
}