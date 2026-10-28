#include "lizard/utils/argparse.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Forward declarations for functions used before definition */
void lz_argparse_print_help(const LzArgParser *parser, FILE *stream);
void lz_argparse_print_version(const LzArgParser *parser, FILE *stream);

/* Internal state for parsing */
typedef struct {
    const LzArgParser *parser;
    int argc;
    char **argv;
    int pos;
    bool help_requested;
    bool version_requested;
    const char **positional_args;
    size_t positional_count;
    size_t positional_capacity;
    char error_buffer[512];
} ParseContext;

static inline const char *get_color(const char *color, bool enabled) {
    if (!enabled || color == NULL) return "";
    return color;
}

/* Calculate the display width of a string, ignoring ANSI escape codes */
static size_t calc_display_width(const char *str) {
    size_t width = 0;
    bool in_escape = false;

    for (const char *p = str; *p; p++) {
        if (*p == '\033' && *(p + 1) == '[') {
            in_escape = true;
            continue;
        }
        if (in_escape) {
            if (*p >= 'A' && *p <= 'Z') {
                in_escape = false;
            } else if (*p >= 'a' && *p <= 'z') {
                in_escape = false;
            }
            continue;
        }
        width++;
    }
    return width;
}

/* Print a string with padding to achieve target display width */
static void fprint_padded(FILE *stream, const char *str, size_t target_width) {
    size_t current_width = calc_display_width(str);
    fputs(str, stream);
    if (current_width < target_width) {
        for (size_t i = 0; i < target_width - current_width; i++) {
            fputc(' ', stream);
        }
    }
}

/* Build the option string for display and return its display width */
static size_t build_opt_string(const LzArg *arg, char *buf, size_t buf_size, bool color) {
    const char *cyan = get_color(LZ_COLOR_CYAN, color);
    const char *reset = get_color(LZ_COLOR_RESET, color);
    const char *yellow = get_color(LZ_COLOR_YELLOW, color);

    char *p = buf;
    size_t remaining = buf_size - 1;

    p += snprintf(p, remaining, "  ");
    remaining = buf_size - (size_t)(p - buf) - 1;

    if (arg->short_name) {
        p += snprintf(p, remaining, "%s-%s%s", cyan, arg->short_name, reset);
        remaining = buf_size - (size_t)(p - buf) - 1;
        if (arg->long_name) {
            p += snprintf(p, remaining, ", ");
            remaining = buf_size - (size_t)(p - buf) - 1;
        }
    } else {
        p += snprintf(p, remaining, "    ");
        remaining = buf_size - (size_t)(p - buf) - 1;
    }

    if (arg->long_name) {
        p += snprintf(p, remaining, "%s--%s%s", cyan, arg->long_name, reset);
        remaining = buf_size - (size_t)(p - buf) - 1;
    }

    if (arg->type == LZ_ARG_OPTION && arg->metavar) {
        p += snprintf(p, remaining, "=%s<%s>%s", yellow, arg->metavar, reset);
        remaining = buf_size - (size_t)(p - buf) - 1;
    }

    *p = '\0';
    return calc_display_width(buf);
}

static void set_error(ParseContext *ctx, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(ctx->error_buffer, sizeof(ctx->error_buffer), fmt, ap);
    va_end(ap);
}

static bool is_short_opt(const char *arg) {
    return arg[0] == '-' && arg[1] != '-' && arg[1] != '\0';
}

static bool is_long_opt(const char *arg) {
    return arg[0] == '-' && arg[1] == '-' && arg[2] != '\0';
}

static const LzArg *find_arg_by_short(const LzArgParser *parser, char c) {
    for (size_t i = 0; i < parser->num_args; i++) {
        if (parser->args[i].short_name && parser->args[i].short_name[0] == c) {
            return &parser->args[i];
        }
    }
    return NULL;
}

static const LzArg *find_arg_by_long(const LzArgParser *parser, const char *name) {
    for (size_t i = 0; i < parser->num_args; i++) {
        if (parser->args[i].long_name && strcmp(parser->args[i].long_name, name) == 0) {
            return &parser->args[i];
        }
    }
    return NULL;
}

static bool validate_choice(const LzArg *arg, const char *value) {
    if (arg->choices == NULL || arg->num_choices == 0) return true;
    for (size_t i = 0; i < arg->num_choices; i++) {
        if (strcmp(arg->choices[i], value) == 0) return true;
    }
    return false;
}

static void store_value(const LzArg *arg, const char *value) {
    if (arg->dest == NULL) return;

    switch (arg->type) {
        case LZ_ARG_FLAG: {
            bool *dest = (bool *)arg->dest;
            *dest = true;
            break;
        }
        case LZ_ARG_OPTION: {
            const char **dest = (const char **)arg->dest;
            *dest = value;
            break;
        }
        case LZ_ARG_POSITIONAL: {
            const char **dest = (const char **)arg->dest;
            *dest = value;
            break;
        }
        case LZ_ARG_HELP: {
            bool *dest = (bool *)arg->dest;
            *dest = true;
            break;
        }
        case LZ_ARG_VERSION: {
            bool *dest = (bool *)arg->dest;
            *dest = true;
            break;
        }
    }
}

static bool parse_long_option(ParseContext *ctx, const char *arg) {
    const char *eq = strchr(arg + 2, '=');
    const char *name = eq ? arg + 2 : arg + 2;
    size_t name_len = eq ? (size_t)(eq - (arg + 2)) : strlen(arg + 2);

    char name_buf[128];
    if (name_len >= sizeof(name_buf)) {
        set_error(ctx, "option name too long");
        return false;
    }
    memcpy(name_buf, name, name_len);
    name_buf[name_len] = '\0';

    const LzArg *def = find_arg_by_long(ctx->parser, name_buf);
    if (def == NULL) {
        if (ctx->parser->allow_unknown) return true;
        set_error(ctx, "unknown option '--%s'", name_buf);
        return false;
    }

    if (def->type == LZ_ARG_FLAG || def->type == LZ_ARG_HELP || def->type == LZ_ARG_VERSION) {
        if (eq != NULL) {
            set_error(ctx, "option '--%s' does not take a value", name_buf);
            return false;
        }
        if (def->type == LZ_ARG_HELP) ctx->help_requested = true;
        if (def->type == LZ_ARG_VERSION) ctx->version_requested = true;
        store_value(def, "true");
        return true;
    }

    if (def->type == LZ_ARG_OPTION) {
        const char *value = eq ? eq + 1 : NULL;
        if (value == NULL) {
            if (ctx->pos + 1 >= ctx->argc) {
                set_error(ctx, "option '--%s' requires a value", name_buf);
                return false;
            }
            value = ctx->argv[++ctx->pos];
        }
        if (!validate_choice(def, value)) {
            set_error(ctx, "invalid value for '--%s': '%s'", name_buf, value);
            return false;
        }
        store_value(def, value);
        return true;
    }

    set_error(ctx, "internal error: unexpected argument type for '--%s'", name_buf);
    return false;
}

static bool parse_short_options(ParseContext *ctx, const char *arg) {
    for (size_t i = 1; arg[i] != '\0'; i++) {
        char c = arg[i];
        const LzArg *def = find_arg_by_short(ctx->parser, c);
        if (def == NULL) {
            if (ctx->parser->allow_unknown) continue;
            set_error(ctx, "unknown option '-%c'", c);
            return false;
        }

        if (def->type == LZ_ARG_FLAG || def->type == LZ_ARG_HELP || def->type == LZ_ARG_VERSION) {
            if (def->type == LZ_ARG_HELP) ctx->help_requested = true;
            if (def->type == LZ_ARG_VERSION) ctx->version_requested = true;
            store_value(def, "true");
        } else if (def->type == LZ_ARG_OPTION) {
            const char *value = NULL;
            if (arg[i + 1] != '\0') {
                value = &arg[i + 1];
                i = strlen(arg);
            } else if (ctx->pos + 1 < ctx->argc) {
                value = ctx->argv[++ctx->pos];
            } else {
                set_error(ctx, "option '-%c' requires a value", c);
                return false;
            }
            if (!validate_choice(def, value)) {
                set_error(ctx, "invalid value for '-%c': '%s'", c, value);
                return false;
            }
            store_value(def, value);
        } else {
            set_error(ctx, "internal error: unexpected argument type for '-%c'", c);
            return false;
        }
    }
    return true;
}

static void add_positional(ParseContext *ctx, const char *arg) {
    if (ctx->positional_count >= ctx->positional_capacity) {
        size_t new_cap = ctx->positional_capacity == 0 ? 4 : ctx->positional_capacity * 2;
        const char **new_args = realloc(ctx->positional_args, new_cap * sizeof(char *));
        if (new_args == NULL) return;
        ctx->positional_args = new_args;
        ctx->positional_capacity = new_cap;
    }
    ctx->positional_args[ctx->positional_count++] = arg;
}

static void apply_defaults(const LzArgParser *parser) {
    for (size_t i = 0; i < parser->num_args; i++) {
        const LzArg *arg = &parser->args[i];
        if (arg->dest && arg->default_value) {
            switch (arg->type) {
                case LZ_ARG_OPTION: {
                    const char **dest = (const char **)arg->dest;
                    if (*dest == NULL) *dest = arg->default_value;
                    break;
                }
                case LZ_ARG_FLAG: {
                    bool *dest = (bool *)arg->dest;
                    if (!*dest) *dest = false;
                    break;
                }
                default:
                    break;
            }
        }
    }
}

static void assign_positional_args(const LzArgParser *parser, ParseContext *ctx) {
    size_t pos_idx = 0;
    for (size_t i = 0; i < parser->num_args; i++) {
        const LzArg *arg = &parser->args[i];
        if (arg->type == LZ_ARG_POSITIONAL && arg->dest) {
            if (pos_idx < ctx->positional_count) {
                const char **dest = (const char **)arg->dest;
                *dest = ctx->positional_args[pos_idx];
                pos_idx++;
            }
        }
    }
}

static bool check_required(const LzArgParser *parser, ParseContext *ctx) {
    for (size_t i = 0; i < parser->num_args; i++) {
        const LzArg *arg = &parser->args[i];
        if (arg->required && arg->type == LZ_ARG_POSITIONAL) {
            const char **dest = (const char **)arg->dest;
            if (*dest == NULL) {
                set_error(ctx, "missing required argument '%s'", arg->metavar ? arg->metavar : arg->long_name);
                return false;
            }
        }
    }
    return true;
}

void lz_argparse_init(LzArgParser *parser, const char *program_name) {
    memset(parser, 0, sizeof(*parser));
    parser->program_name = program_name;
    parser->color_enabled = true;
    parser->error_prefix = "error";
}

void lz_argparse_set_description(LzArgParser *parser, const char *description) {
    parser->description = description;
}

void lz_argparse_set_epilog(LzArgParser *parser, const char *epilog) {
    parser->epilog = epilog;
}

void lz_argparse_set_version(LzArgParser *parser, const char *version) {
    parser->version = version;
}

void lz_argparse_set_args(LzArgParser *parser, const LzArg *args, size_t num_args) {
    parser->args = args;
    parser->num_args = num_args;
}

void lz_argparse_enable_color(LzArgParser *parser, bool enable) {
    parser->color_enabled = enable;
}

void lz_argparse_allow_unknown(LzArgParser *parser, bool allow) {
    parser->allow_unknown = allow;
}

LzArgParseResult lz_argparse_parse(LzArgParser *parser, int argc, char **argv) {
    ParseContext ctx = {0};
    ctx.parser = parser;
    ctx.argc = argc;
    ctx.argv = argv;
    ctx.pos = 1;

    LzArgParseResult result = {0};

    bool end_of_options = false;

    for (; ctx.pos < argc; ctx.pos++) {
        const char *arg = argv[ctx.pos];

        if (end_of_options) {
            add_positional(&ctx, arg);
            continue;
        }

        if (strcmp(arg, "--") == 0) {
            end_of_options = true;
            continue;
        }

        if (is_long_opt(arg)) {
            if (!parse_long_option(&ctx, arg)) break;
        } else if (is_short_opt(arg)) {
            if (!parse_short_options(&ctx, arg)) break;
        } else {
            add_positional(&ctx, arg);
        }
    }

    if (ctx.error_buffer[0] != '\0') {
        result.success = false;
        strncpy(result.error_buffer, ctx.error_buffer, sizeof(result.error_buffer) - 1);
        result.error_buffer[sizeof(result.error_buffer) - 1] = '\0';
        result.error_message = result.error_buffer;
        result.positional_args = ctx.positional_args;
        result.positional_count = (int)ctx.positional_count;
        return result;
    }

    apply_defaults(parser);
    assign_positional_args(parser, &ctx);

    /* If help or version was requested, print and return success immediately
     * without validating required arguments. This allows --help to work even
     * when required arguments are missing. */
    if (ctx.help_requested) {
        lz_argparse_print_help(parser, stdout);
        result.success = true;
        result.positional_args = ctx.positional_args;
        result.positional_count = (int)ctx.positional_count;
        result.error_message = NULL;
        return result;
    }

    if (ctx.version_requested && parser->version) {
        lz_argparse_print_version(parser, stdout);
        result.success = true;
        result.positional_args = ctx.positional_args;
        result.positional_count = (int)ctx.positional_count;
        result.error_message = NULL;
        return result;
    }

    if (!check_required(parser, &ctx)) {
        result.success = false;
        strncpy(result.error_buffer, ctx.error_buffer, sizeof(result.error_buffer) - 1);
        result.error_buffer[sizeof(result.error_buffer) - 1] = '\0';
        result.error_message = result.error_buffer;
        result.positional_args = ctx.positional_args;
        result.positional_count = (int)ctx.positional_count;
        return result;
    }

    result.success = true;
    result.positional_args = ctx.positional_args;
    result.positional_count = (int)ctx.positional_count;
    result.error_message = NULL;

    return result;
}

void lz_argparse_print_help(const LzArgParser *parser, FILE *stream) {
    bool color = parser->color_enabled;
    const char *bold = get_color(LZ_COLOR_BOLD, color);
    const char *reset = get_color(LZ_COLOR_RESET, color);
    const char *yellow = get_color(LZ_COLOR_YELLOW, color);
    const char *gray = get_color(LZ_COLOR_GRAY, color);
    const char *green = get_color(LZ_COLOR_GREEN, color);

    fprintf(stream, "%sUsage:%s %s [options]", bold, reset, parser->program_name);

    bool has_positionals = false;
    for (size_t i = 0; i < parser->num_args; i++) {
        if (parser->args[i].type == LZ_ARG_POSITIONAL) {
            has_positionals = true;
            if (parser->args[i].required) {
                fprintf(stream, " %s<%s>%s", yellow, parser->args[i].metavar ? parser->args[i].metavar : parser->args[i].long_name, reset);
            } else {
                fprintf(stream, " %s[%s]%s", yellow, parser->args[i].metavar ? parser->args[i].metavar : parser->args[i].long_name, reset);
            }
        }
    }
    fputc('\n', stream);

    if (parser->description) {
        fputc('\n', stream);
        fputs(parser->description, stream);
        fputc('\n', stream);
    }

    bool has_options = false;
    for (size_t i = 0; i < parser->num_args; i++) {
        if (parser->args[i].type != LZ_ARG_POSITIONAL) {
            has_options = true;
            break;
        }
    }

    if (has_options) {
        /* First pass: calculate max option string display width */
        size_t max_opt_width = 0;
        char opt_str[128];
        for (size_t i = 0; i < parser->num_args; i++) {
            const LzArg *arg = &parser->args[i];
            if (arg->type == LZ_ARG_POSITIONAL) continue;
            size_t width = build_opt_string(arg, opt_str, sizeof(opt_str), color);
            if (width > max_opt_width) max_opt_width = width;
        }

        /* Ensure minimum width and add padding */
        if (max_opt_width < 24) max_opt_width = 24;
        max_opt_width += 2;  /* Add 2 spaces padding */

        fprintf(stream, "\n%sOptions:%s\n", bold, reset);
        for (size_t i = 0; i < parser->num_args; i++) {
            const LzArg *arg = &parser->args[i];
            if (arg->type == LZ_ARG_POSITIONAL) continue;

            build_opt_string(arg, opt_str, sizeof(opt_str), color);
            fprint_padded(stream, opt_str, max_opt_width);

            if (arg->description) {
                fprintf(stream, "%s", arg->description);
            }

            if (arg->choices && arg->num_choices > 0) {
                fprintf(stream, " %s(", gray);
                for (size_t j = 0; j < arg->num_choices; j++) {
                    if (j > 0) fprintf(stream, "|");
                    fprintf(stream, "%s", arg->choices[j]);
                }
                fprintf(stream, ")%s", reset);
            }

            if (arg->default_value && arg->type == LZ_ARG_OPTION) {
                fprintf(stream, " %s(default: %s)%s", gray, arg->default_value, reset);
            }

            fputc('\n', stream);
        }
    }

    if (has_positionals) {
        /* Calculate max argument string width for alignment */
        size_t max_arg_width = 0;
        char arg_str[128];
        for (size_t i = 0; i < parser->num_args; i++) {
            const LzArg *arg = &parser->args[i];
            if (arg->type != LZ_ARG_POSITIONAL) continue;
            snprintf(arg_str, sizeof(arg_str), "  %s%s%s",
                     yellow,
                     arg->metavar ? arg->metavar : arg->long_name,
                     reset);
            size_t width = calc_display_width(arg_str);
            if (width > max_arg_width) max_arg_width = width;
        }

        if (max_arg_width < 24) max_arg_width = 24;
        max_arg_width += 2;

        fprintf(stream, "\n%sArguments:%s\n", bold, reset);
        for (size_t i = 0; i < parser->num_args; i++) {
            const LzArg *arg = &parser->args[i];
            if (arg->type != LZ_ARG_POSITIONAL) continue;

            snprintf(arg_str, sizeof(arg_str), "  %s%s%s",
                     yellow,
                     arg->metavar ? arg->metavar : arg->long_name,
                     reset);

            fprint_padded(stream, arg_str, max_arg_width);

            if (arg->description) {
                fprintf(stream, "%s", arg->description);
            }

            if (arg->required) {
                fprintf(stream, " %s(required)%s", green, reset);
            }

            if (arg->default_value) {
                fprintf(stream, " %s(default: %s)%s", gray, arg->default_value, reset);
            }

            fputc('\n', stream);
        }
    }

    if (parser->epilog) {
        fputc('\n', stream);
        fputs(parser->epilog, stream);
        fputc('\n', stream);
    }
}

void lz_argparse_print_version(const LzArgParser *parser, FILE *stream) {
    if (parser->version) {
        fprintf(stream, "%s\n", parser->version);
    }
}

void lz_argparse_free_result(LzArgParseResult *result) {
    if (result->positional_args) {
        free(result->positional_args);
        result->positional_args = NULL;
        result->positional_count = 0;
    }
}

/* Parse a count string with optional K/M/G suffix (powers of 1024) */
bool lz_parse_count(const char *text, bool allow_suffix, uint64_t *out) {
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