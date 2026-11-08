#include "lizard/report.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define TAB_WIDTH 4u

/* Colours: bold red, bright blue, bold green, yellow, cyan, bold. */
#define C_RESET "\x1b[0m"
#define C_BOLD "\x1b[1m"
#define C_RED "\x1b[1;31m"
#define C_YELLOW "\x1b[1;33m"
#define C_BLUE "\x1b[1;94m"
#define C_GREEN "\x1b[1;32m"
#define C_CYAN "\x1b[1;36m"

typedef struct Out {
    const LzWriter *writer;
    bool color;
} Out;

static void raw(const Out *out, const char *text, size_t length) {
    if (length > 0) {
        (void)lz_writer_write(out->writer, text, length);
    }
}

static void styled(const Out *out, const char *style, const char *text, size_t length) {
    if (out->color && style != NULL && length > 0) {
        (void)lz_writer_write_cstr(out->writer, style);
        raw(out, text, length);
        (void)lz_writer_write_cstr(out->writer, C_RESET);
    } else {
        raw(out, text, length);
    }
}

static void styled_cstr(const Out *out, const char *style, const char *text) {
    styled(out, style, text, strlen(text));
}

static void spaces(const Out *out, size_t count) {
    static const char blank[] = "                                ";
    while (count > 0) {
        size_t chunk = count < sizeof blank - 1 ? count : sizeof blank - 1;
        raw(out, blank, chunk);
        count -= chunk;
    }
}

static void repeat(const Out *out, const char *style, char c, size_t count) {
    char buffer[64];
    while (count > 0) {
        size_t chunk = count < sizeof buffer ? count : sizeof buffer;
        memset(buffer, c, chunk);
        styled(out, style, buffer, chunk);
        count -= chunk;
    }
}

static void put_unsigned(const Out *out, const char *style, size_t value) {
    char digits[24];
    int length = snprintf(digits, sizeof digits, "%zu", value);
    if (length > 0) {
        styled(out, style, digits, (size_t)length);
    }
}

static bool is_continuation(char byte) {
    return ((unsigned char)byte & 0xC0) == 0x80;
}

/* Screen columns of text[from, to): code points, a tab counts TAB_WIDTH. */
static size_t columns(const char *text, size_t from, size_t to) {
    size_t width = 0;
    for (size_t i = from; i < to; i++) {
        if (text[i] == '\t') {
            width += TAB_WIDTH;
        } else if (!is_continuation(text[i])) {
            width++;
        }
    }
    return width;
}

/* Writes text[from, to) with tabs widened, so the marker line below lines up. */
static void source_text(const Out *out, const char *style, const char *text, size_t from,
                        size_t to) {
    size_t start = from;
    for (size_t i = from; i < to; i++) {
        if (text[i] == '\t') {
            styled(out, style, text + start, i - start);
            for (unsigned k = 0; k < TAB_WIDTH; k++) {
                styled(out, style, " ", 1);
            }
            start = i + 1;
        }
    }
    styled(out, style, text + start, to - start);
}

static size_t digit_count(size_t value) {
    size_t digits = 1;
    while (value >= 10) {
        value /= 10;
        digits++;
    }
    return digits;
}

/* "  |" : the empty gutter line. */
static void gutter(const Out *out, size_t width) {
    spaces(out, width + 1);
    styled_cstr(out, C_BLUE, "|");
}

static void gutter_line(const Out *out, size_t width) {
    gutter(out, width);
    raw(out, "\n", 1);
}

static void severity_color(const char *severity, const char **style) {
    *style = strcmp(severity, "warning") == 0 ? C_YELLOW : C_RED;
}

static void header(const Out *out, const char *severity, const char *message) {
    const char *style;
    severity_color(severity, &style);
    styled_cstr(out, style, severity);
    styled(out, C_BOLD, ": ", 2);
    styled_cstr(out, C_BOLD, message);
    raw(out, "\n", 1);
}

static void put_line_number(const Out *out, size_t number, size_t width);

/* The corrected line with the marker line below it. */
static void write_suggestion(const Out *out, const LzSource *source, const LzLocation *where,
                             size_t offset, size_t span, const char *candidate, size_t width,
                             size_t line_number, bool first) {
    const char *typed = source->text + offset;
    size_t candidate_length = strlen(candidate);

    /* What differs between the typed word and the candidate. */
    size_t prefix = 0;
    while (prefix < span && prefix < candidate_length && typed[prefix] == candidate[prefix]) {
        prefix++;
    }
    size_t suffix = 0;
    while (suffix < span - prefix && suffix < candidate_length - prefix &&
           typed[span - 1 - suffix] == candidate[candidate_length - 1 - suffix]) {
        suffix++;
    }
    size_t added = candidate_length - prefix - suffix;
    size_t removed = span - prefix - suffix;

    styled_cstr(out, C_GREEN, "help");
    styled(out, C_BOLD, ": ", 2);
    styled_cstr(out, C_BOLD, first ? "did you mean `" : "or maybe `");
    styled_cstr(out, C_GREEN, candidate);
    styled_cstr(out, C_BOLD, "`?");
    raw(out, "\n", 1);

    gutter_line(out, width);

    /* The corrected line; the changed letters stand out in green. */
    put_line_number(out, line_number, width);
    raw(out, " ", 1);
    styled_cstr(out, C_BLUE, "|");
    raw(out, " ", 1);
    source_text(out, NULL, source->text, where->line_start, offset);
    source_text(out, NULL, candidate, 0, prefix);
    if (out->color) {
        styled(out, C_GREEN, candidate + prefix, added);
    } else {
        raw(out, candidate + prefix, added);
    }
    source_text(out, NULL, candidate, candidate_length - suffix, candidate_length);
    source_text(out, NULL, source->text, offset + span, where->line_end);
    raw(out, "\n", 1);

    /* The marker line. */
    gutter(out, width);
    raw(out, " ", 1);
    spaces(out, columns(source->text, where->line_start, offset) + columns(candidate, 0, prefix));
    if (added > 0) {
        repeat(out, C_GREEN, removed > 0 ? '~' : '+', columns(candidate, prefix, prefix + added));
    } else if (removed > 0) {
        repeat(out, C_RED, '-', 1);
        raw(out, " ", 1);
        styled_cstr(out, C_RED, "remove `");
        styled(out, C_RED, typed + prefix, removed);
        styled_cstr(out, C_RED, "`");
    }
    raw(out, "\n", 1);
}

/* The line number, padded on the right to the gutter width. */
static void put_line_number(const Out *out, size_t number, size_t width) {
    put_unsigned(out, C_BLUE, number);
    spaces(out, width - digit_count(number));
}

static size_t clamp_offset(const LzSource *source, size_t offset) {
    return offset > source->length ? source->length : offset;
}

/* The place a fix edits: its file and the location in it. */
static const LzSource *fix_source(const LzSource *error_source, const LzFix *fix) {
    return fix->source != NULL ? fix->source : error_source;
}

/*
 * help: <headline>, then the corrected line (or the new line) with markers under what changed.
 * `+` added, `~` replaced, `-` removed; a whole inserted line has `+` in the margin.
 */
static void write_fix(const Out *out, const LzSource *error_source, const LzLocation *error_where,
                      const LzFix *fix, size_t width) {
    const LzSource *source = fix_source(error_source, fix);
    size_t offset = clamp_offset(source, fix->offset);
    LzLocation where = lz_source_locate(source, offset);
    const char *insert = fix->insert_text != NULL ? fix->insert_text : "";
    size_t insert_length = strlen(insert);

    styled_cstr(out, C_GREEN, "help");
    styled(out, C_BOLD, ": ", 2);
    styled_cstr(out, C_BOLD, fix->headline != NULL ? fix->headline : "change this");
    raw(out, "\n", 1);

    bool same_place = source == error_source && where.line == error_where->line;
    if (!same_place) {
        spaces(out, width);
        styled_cstr(out, C_BLUE, "--> ");
        raw(out, source->name, strlen(source->name));
        raw(out, ":", 1);
        put_unsigned(out, NULL, where.line);
        raw(out, ":", 1);
        put_unsigned(out, NULL, where.column);
        raw(out, "\n", 1);
    }
    gutter_line(out, width);

    if (fix->insert_line) {
        /* The new line, then the line it goes in front of. */
        size_t indent_end = where.line_start;
        while (indent_end < where.line_end &&
               (source->text[indent_end] == ' ' || source->text[indent_end] == '\t')) {
            indent_end++;
        }
        put_line_number(out, where.line, width);
        raw(out, " ", 1);
        styled_cstr(out, C_GREEN, "+");
        raw(out, " ", 1);
        if (fix->insert_indent != NULL) {
            source_text(out, out->color ? C_GREEN : NULL, fix->insert_indent, 0,
                        strlen(fix->insert_indent));
        } else {
            source_text(out, out->color ? C_GREEN : NULL, source->text, where.line_start, indent_end);
        }
        source_text(out, out->color ? C_GREEN : NULL, insert, 0, insert_length);
        raw(out, "\n", 1);

        put_line_number(out, where.line, width);
        raw(out, " ", 1);
        styled_cstr(out, C_BLUE, "|");
        raw(out, " ", 1);
        source_text(out, NULL, source->text, where.line_start, where.line_end);
        raw(out, "\n", 1);
        return;
    }

    /* The edit is clipped to its line. */
    size_t removed = fix->delete_length;
    if (offset + removed > where.line_end) {
        removed = where.line_end > offset ? where.line_end - offset : 0;
    }
    put_line_number(out, where.line, width);
    raw(out, " ", 1);
    styled_cstr(out, C_BLUE, "|");
    raw(out, " ", 1);
    source_text(out, NULL, source->text, where.line_start, offset);
    source_text(out, out->color ? C_GREEN : NULL, insert, 0, insert_length);
    source_text(out, NULL, source->text, offset + removed, where.line_end);
    raw(out, "\n", 1);

    gutter(out, width);
    raw(out, " ", 1);
    spaces(out, columns(source->text, where.line_start, offset));
    if (insert_length > 0) {
        repeat(out, C_GREEN, removed > 0 ? '~' : '+', columns(insert, 0, insert_length));
    } else if (removed > 0) {
        repeat(out, C_RED, '-', 1);
        raw(out, " ", 1);
        styled_cstr(out, C_RED, "remove `");
        styled(out, C_RED, source->text + offset, removed);
        styled_cstr(out, C_RED, "`");
    }
    raw(out, "\n", 1);
}

/*
 * Text that may span lines: the first line follows the caller's prefix, each later line
 * (a piece of code to copy, usually) is indented by `indent` columns.
 */
static void write_text_block(const Out *out, const char *text, size_t indent) {
    const char *line = text;
    bool first = true;
    while (true) {
        const char *end = strchr(line, '\n');
        size_t length = end != NULL ? (size_t)(end - line) : strlen(line);
        if (!first) {
            raw(out, "\n", 1);
            spaces(out, indent);
        }
        raw(out, line, length);
        if (end == NULL) {
            break;
        }
        line = end + 1;
        first = false;
    }
}

void lz_report_write(const LzSource *source, const LzReport *report, bool color,
                     const LzWriter *out_writer) {
    Out out = {out_writer, color};
    const char *severity = report->severity != NULL ? report->severity : "error";

    size_t offset = report->offset > source->length ? source->length : report->offset;
    LzLocation where = lz_source_locate(source, offset);
    size_t width = digit_count(where.line);
    if (report->fix != NULL) {
        const LzSource *fsrc = fix_source(source, report->fix);
        LzLocation fwhere = lz_source_locate(fsrc, clamp_offset(fsrc, report->fix->offset));
        if (digit_count(fwhere.line) > width) {
            width = digit_count(fwhere.line);
        }
    }

    header(&out, severity, report->message);

    /*  --> file:line:col */
    spaces(&out, width);
    styled_cstr(&out, C_BLUE, "--> ");
    raw(&out, source->name, strlen(source->name));
    raw(&out, ":", 1);
    put_unsigned(&out, NULL, where.line);
    raw(&out, ":", 1);
    put_unsigned(&out, NULL, where.column);
    raw(&out, "\n", 1);

    gutter_line(&out, width);

    /* The source line. */
    put_line_number(&out, where.line, width);
    raw(&out, " ", 1);
    styled_cstr(&out, C_BLUE, "|");
    raw(&out, " ", 1);
    source_text(&out, NULL, source->text, where.line_start, where.line_end);
    raw(&out, "\n", 1);

    /* The carets, clipped to the first line of the span. */
    size_t span_end = offset + report->length;
    if (span_end > where.line_end) {
        span_end = where.line_end;
    }
    size_t span = span_end > offset ? span_end - offset : 0;
    size_t carets = columns(source->text, offset, offset + span);
    if (carets == 0) {
        carets = 1;
    }
    const char *style;
    severity_color(severity, &style);
    gutter(&out, width);
    raw(&out, " ", 1);
    spaces(&out, columns(source->text, where.line_start, offset));
    repeat(&out, style, '^', carets);
    if (report->label != NULL && report->label[0] != '\0') {
        raw(&out, " ", 1);
        styled_cstr(&out, style, report->label);
    }
    raw(&out, "\n", 1);

    bool has_suggestions = report->suggestions != NULL && report->suggestions->count > 0 && span > 0;
    bool has_footer = report->note != NULL || report->help != NULL ||
                      (report->frames != NULL && report->frame_count > 0);
    if (has_suggestions) {
        gutter_line(&out, width);
    }

    if (has_suggestions) {
        for (size_t i = 0; i < report->suggestions->count; i++) {
            if (i > 0) {
                gutter_line(&out, width);
            }
            write_suggestion(&out, source, &where, offset, span, report->suggestions->names[i],
                             width, where.line, i == 0);
        }
    }

    if (has_footer) {
        gutter_line(&out, width);
    }
    if (report->note != NULL) {
        spaces(&out, width + 1);
        styled_cstr(&out, C_BLUE, "=");
        raw(&out, " ", 1);
        styled_cstr(&out, C_BOLD, "note");
        styled(&out, C_BOLD, ": ", 2);
        raw(&out, report->note, strlen(report->note));
        raw(&out, "\n", 1);
    }
    if (report->help != NULL) {
        spaces(&out, width + 1);
        styled_cstr(&out, C_BLUE, "=");
        raw(&out, " ", 1);
        styled_cstr(&out, C_GREEN, "help");
        styled(&out, C_BOLD, ": ", 2);
        write_text_block(&out, report->help, width + 1 + 2 + 6 + 4);
        raw(&out, "\n", 1);
    }
    if (report->fix != NULL) {
        if (!has_footer) {
            gutter_line(&out, width);
        }
        write_fix(&out, source, &where, report->fix, width);
        if (report->frames != NULL && report->frame_count > 0) {
            gutter_line(&out, width);
        }
    }
    if (report->frames != NULL && report->frame_count > 0) {
        spaces(&out, width + 1);
        styled_cstr(&out, C_BLUE, "=");
        raw(&out, " ", 1);
        styled_cstr(&out, C_CYAN, "stack trace");
        styled(&out, C_BOLD, ": ", 2);
        raw(&out, "innermost call first\n", 21);
        for (size_t i = 0; i < report->frame_count; i++) {
            const LzReportFrame *frame = &report->frames[i];
            spaces(&out, width + 5);
            raw(&out, "at ", 3);
            styled_cstr(&out, C_BOLD, frame->name);
            if (frame->has_location) {
                raw(&out, " (", 2);
                raw(&out, source->name, strlen(source->name));
                raw(&out, ":", 1);
                put_unsigned(&out, NULL, frame->line);
                raw(&out, ":", 1);
                put_unsigned(&out, NULL, frame->column);
                raw(&out, ")", 1);
            }
            raw(&out, "\n", 1);
        }
        if (report->frames_omitted > 0) {
            spaces(&out, width + 5);
            raw(&out, "... ", 4);
            put_unsigned(&out, NULL, report->frames_omitted);
            raw(&out, " more frames\n", 13);
        }
    }
    for (size_t i = 0; report->causes != NULL && i < report->cause_count; i++) {
        spaces(&out, width + 1);
        styled_cstr(&out, C_BLUE, "=");
        raw(&out, " ", 1);
        styled_cstr(&out, C_CYAN, "caused by");
        styled(&out, C_BOLD, ": ", 2);
        raw(&out, report->causes[i], strlen(report->causes[i]));
        raw(&out, "\n", 1);
    }
    if (report->causes_truncated) {
        spaces(&out, width + 1);
        styled_cstr(&out, C_BLUE, "=");
        raw(&out, " ...\n", 5);
    }

    raw(&out, "\n", 1);
}

void lz_report_write_summary(size_t errors, size_t not_shown, bool color, const LzWriter *out_writer) {
    Out out = {out_writer, color};
    if (errors == 0) {
        return;
    }
    styled_cstr(&out, C_RED, "error");
    styled(&out, C_BOLD, ": aborting due to ", 18);
    put_unsigned(&out, C_BOLD, errors);
    styled_cstr(&out, C_BOLD, errors == 1 ? " previous error" : " previous errors");
    if (not_shown > 0) {
        styled(&out, C_BOLD, " (", 2);
        put_unsigned(&out, C_BOLD, not_shown);
        styled_cstr(&out, C_BOLD, " more not shown)");
    }
    raw(&out, "\n", 1);
}
