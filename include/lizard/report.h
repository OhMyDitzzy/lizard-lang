#ifndef LIZARD_REPORT_H
#define LIZARD_REPORT_H

#include "lizard/common.h"
#include "lizard/source.h"
#include "lizard/suggest.h"
#include "lizard/writer.h"

/* One line of a stack trace, already resolved to a position. */
typedef struct LzReportFrame {
    const char *name;
    bool has_location;
    uint32_t line;
    uint32_t column;
} LzReportFrame;

/*
 * A visual fix: a small edit to the source, drawn as the corrected line with markers under
 * what changed (`+` added, `~` replaced, `-` removed). All text is borrowed.
 *
 *   help: make the field a link: it points at an object without owning it
 *     --> class.lz:3:9
 *      |
 *   3  |     pub link relation = nil
 *      |         +++++
 */
typedef struct LzFix {
    const char *headline;      /* "make the field a link" */
    const LzSource *source;    /* the file the fix is in (NULL = the error's own file) */
    size_t offset;             /* where to edit */
    size_t delete_length;      /* bytes removed (0 = pure insertion) */
    const char *insert_text;   /* text inserted at offset (may be "") */
    bool insert_line;          /* the text is a whole new line placed before the line of `offset` */
    const char *insert_indent; /* indentation of that new line, or NULL = same as the line of offset */
} LzFix;

/*
 * Everything one error report shows. Text fields are borrowed and optional
 * (NULL leaves that part out).
 *
 *   error: undefined function 'printj'
 *    --> hello.lz:2:1
 *     |
 *   2 | printj("Hello")
 *     | ^^^^^^ not found in this scope
 *     |
 *   help: did you mean `println`?
 *     |
 *   2 | println("Hello")
 *     |      ~~
 *
 * Each suggestion replaces the source span [offset, offset + length) and is
 * drawn as the corrected line with the changed letters marked underneath:
 * `+` for letters to add, `~` for letters to change, `-` for letters to remove.
 */
typedef struct LzReport {
    const char *severity; /* "error" or "warning" */
    const char *message;
    size_t offset;
    size_t length;
    const char *label; /* written next to the carets */
    const char *note;  /* "= note: ..." */
    const char *help;  /* "= help: ..." (a line break starts a line of code to copy) */
    const LzFix *fix;  /* optional: "help: ..." with the corrected line drawn */
    const LzSuggestions *suggestions;
    const LzReportFrame *frames; /* a stack trace, innermost first */
    size_t frame_count;
    size_t frames_omitted;
    const char *const *causes; /* "Kind: message" of each cause, nearest first: "= caused by:" lines */
    size_t cause_count;
    bool causes_truncated; /* the chain goes on beyond the ones given: a "..." line */
} LzReport;

/* Writes one report followed by a blank line. color adds ANSI escape sequences. */
void lz_report_write(const LzSource *source, const LzReport *report, bool color,
                     const LzWriter *out);

/* "error: aborting due to N previous errors" (plus how many were not shown). */
void lz_report_write_summary(size_t errors, size_t not_shown, bool color, const LzWriter *out);

#endif
