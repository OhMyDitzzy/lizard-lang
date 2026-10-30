#include <stdlib.h>
#include <string.h>

#include "lizard/builtin.h"
#include "lizard/compiler.h"
#include "lizard/report.h"
#include "lizard/suggest.h"
#include "test_util.h"

static void test_edit_distance(void) {
    LZ_CHECK(lz_edit_distance("print", 5, "print", 5) == 0);
    LZ_CHECK(lz_edit_distance("printj", 6, "print", 5) == 1);
    LZ_CHECK(lz_edit_distance("printj", 6, "println", 7) == 2);
    LZ_CHECK(lz_edit_distance("pritnln", 7, "println", 7) == 1); /* a swap is one edit */
    LZ_CHECK(lz_edit_distance("Println", 7, "println", 7) == 0); /* case is ignored */
    LZ_CHECK(lz_edit_distance("", 0, "abc", 3) == 3);
}

static LzSuggestions suggest(const char *typed, const char *const *names, size_t count) {
    LzSuggester suggester;
    lz_suggester_init(&suggester, typed, strlen(typed));
    for (size_t i = 0; i < count; i++) {
        lz_suggester_offer(&suggester, names[i]);
    }
    return suggester.found;
}

static void test_ranking(void) {
    const char *builtins[] = {"len", "print", "println", "push", "sort", "println"};
    LzSuggestions found = suggest("printj", builtins, 6);
    LZ_CHECK(found.count == 2); /* the duplicate is ignored */
    LZ_CHECK(strcmp(found.names[0], "print") == 0);
    LZ_CHECK(strcmp(found.names[1], "println") == 0);

    /* A wrong case is a certain match: weaker candidates are not offered next to it. */
    found = suggest("Println", builtins, 6);
    LZ_CHECK(found.count == 1 && strcmp(found.names[0], "println") == 0);

    /* Nothing close: no suggestion. */
    found = suggest("zzzzzz", builtins, 6);
    LZ_CHECK(found.count == 0);

    /* Short names do not match each other (every `x` would suggest `y`)... */
    const char *letters[] = {"x", "y", "xs"};
    LZ_CHECK(suggest("z", letters, 3).count == 0);
    LZ_CHECK(suggest("xy", letters, 3).count == 0);
    /* ...unless only the case differs. */
    const char *upper[] = {"x"};
    LZ_CHECK(suggest("X", upper, 1).count == 1);

    /* The typed name itself is never a suggestion. */
    const char *same[] = {"println"};
    LZ_CHECK(suggest("println", same, 1).count == 0);

    /* At most LZ_SUGGEST_MAX. */
    const char *many[] = {"item1", "item2", "item3", "item4", "item5"};
    LZ_CHECK(suggest("item", many, 5).count == LZ_SUGGEST_MAX);
}

typedef struct Compiled {
    LzVM vm;
    LzBuffer out;
    LzBuffer err;
    LzChunk chunk;
    LzDiagnostics diagnostics;
    LzSource source;
} Compiled;

static void compile(Compiled *c, const char *text) {
    memset(c, 0, sizeof *c);
    lz_vm_init(&c->vm, lz_writer_from_buffer(&c->out), lz_writer_from_buffer(&c->err));
    LZ_CHECK(lz_builtin_install(&c->vm) == LZ_OK);
    lz_source_from_string(&c->source, "t.lz", text);
    lz_chunk_init(&c->chunk);
    (void)lz_compile(&c->vm, &c->source, &c->chunk, &c->diagnostics);
}

static void release(Compiled *c) {
    lz_chunk_free(&c->vm.heap, &c->chunk);
    lz_buffer_free(&c->out);
    lz_buffer_free(&c->err);
    lz_vm_free(&c->vm);
}

/* The hint of the first undefined name in the text, or NULL. */
static const LzNameHint *first_hint(const Compiled *c) {
    return c->diagnostics.hint_count > 0 ? &c->diagnostics.hints[0] : NULL;
}

static bool offers(const LzSuggestions *found, const char *name) {
    for (size_t i = 0; i < found->count; i++) {
        if (strcmp(found->names[i], name) == 0) {
            return true;
        }
    }
    return false;
}

static void test_builtin_hint(void) {
    Compiled c;
    compile(&c, "println(1)\nprintj(2)\n");
    const LzNameHint *hint = first_hint(&c);
    LZ_CHECK(hint != NULL);
    if (hint != NULL) {
        LZ_CHECK(hint->is_call && !hint->is_assignment);
        LZ_CHECK(hint->length == 6);
        LZ_CHECK(offers(&hint->suggestions, "println"));
        LZ_CHECK(offers(&hint->suggestions, "print"));
        LZ_CHECK(lz_diagnostics_find_hint(&c.diagnostics, hint->offset) == hint);
    }
    release(&c);
}

static void test_script_functions_and_locals(void) {
    Compiled c;
    /* A function of the script itself, declared later than its use. */
    compile(&c, "calculate_totl(2)\nfn calculate_total(p) { return p }\n");
    const LzNameHint *hint = first_hint(&c);
    LZ_CHECK(hint != NULL && hint->suggestions.count == 1);
    if (hint != NULL && hint->suggestions.count == 1) {
        LZ_CHECK(strcmp(hint->suggestions.names[0], "calculate_total") == 0);
    }
    release(&c);

    /* A local of the enclosing function. */
    compile(&c, "fn work() {\n  let inner_value = 5\n  println(inner_valeu)\n}\n");
    hint = first_hint(&c);
    LZ_CHECK(hint != NULL && !hint->is_call);
    if (hint != NULL) {
        LZ_CHECK(offers(&hint->suggestions, "inner_value"));
    }
    release(&c);

    /* A call is only matched with things that can be called: `total` holds an int. */
    compile(&c, "let total: int = 1\nfn totals() { return 2 }\ntotalz()\n");
    hint = first_hint(&c);
    LZ_CHECK(hint != NULL && hint->is_call);
    if (hint != NULL) {
        LZ_CHECK(offers(&hint->suggestions, "totals"));
        LZ_CHECK(!offers(&hint->suggestions, "total"));
    }
    release(&c);

    /* Assignment to an undefined name. */
    compile(&c, "let counter = 0\ncuonter = 1\n");
    hint = first_hint(&c);
    LZ_CHECK(hint != NULL && hint->is_assignment);
    if (hint != NULL) {
        LZ_CHECK(offers(&hint->suggestions, "counter"));
    }
    release(&c);
}

static void test_compile_error_suggestions(void) {
    Compiled c;
    compile(&c, "let x: intt = 1\n");
    LZ_CHECK(c.diagnostics.count == 1);
    LZ_CHECK(offers(&c.diagnostics.items[0].suggestions, "int"));
    release(&c);

    compile(&c, "type Mode = \"r\" | \"w\"\nlet m: Mod = \"r\"\n");
    LZ_CHECK(c.diagnostics.count == 1);
    LZ_CHECK(offers(&c.diagnostics.items[0].suggestions, "Mode"));
    release(&c);

    compile(&c, "fn area(w, height = 1) { return w * height }\narea(2, hieght = 3)\n");
    LZ_CHECK(c.diagnostics.count == 1);
    LZ_CHECK(strstr(c.diagnostics.items[0].message, "unexpected named argument 'hieght'") != NULL);
    LZ_CHECK(offers(&c.diagnostics.items[0].suggestions, "height"));
    release(&c);
}

static bool report_is(const char *text, const LzReport *report, const char *expected) {
    LzSource source;
    lz_source_from_string(&source, "t.lz", text);
    LzBuffer buffer = {0};
    LzWriter writer = lz_writer_from_buffer(&buffer);
    lz_report_write(&source, report, false, &writer);
    bool same = buffer.data != NULL && strcmp(buffer.data, expected) == 0;
    if (!same) {
        fprintf(stderr, "got:\n%s\nexpected:\n%s\n", buffer.data != NULL ? buffer.data : "(null)",
                expected);
    }
    lz_buffer_free(&buffer);
    return same;
}

static void test_report_layout(void) {
    LzSuggestions found = {0};
    found.count = 2;
    strcpy(found.names[0], "print");
    strcpy(found.names[1], "println");
    LzReport report = {0};
    report.severity = "error";
    report.message = "undefined function 'printj'";
    report.offset = 11;
    report.length = 6;
    report.label = "not found in this scope";
    report.suggestions = &found;
    LZ_CHECK(report_is("println(1)\nprintj(\"Hi\")\n", &report,
                       "error: undefined function 'printj'\n"
                       " --> t.lz:2:1\n"
                       "  |\n"
                       "2 | printj(\"Hi\")\n"
                       "  | ^^^^^^ not found in this scope\n"
                       "  |\n"
                       "help: did you mean `print`?\n"
                       "  |\n"
                       "2 | print(\"Hi\")\n"
                       "  |      - remove `j`\n"
                       "  |\n"
                       "help: or maybe `println`?\n"
                       "  |\n"
                       "2 | println(\"Hi\")\n"
                       "  |      ~~\n"
                       "\n"));

    /* An insertion is marked with +. */
    found.count = 1;
    strcpy(found.names[0], "println");
    report.message = "undefined function 'printn'";
    report.offset = 0;
    report.length = 6;
    LZ_CHECK(report_is("printn(1)\n", &report,
                       "error: undefined function 'printn'\n"
                       " --> t.lz:1:1\n"
                       "  |\n"
                       "1 | printn(1)\n"
                       "  | ^^^^^^ not found in this scope\n"
                       "  |\n"
                       "help: did you mean `println`?\n"
                       "  |\n"
                       "1 | println(1)\n"
                       "  |      +\n"
                       "\n"));

    /* A tab is widened so the markers line up; a plain report has no suggestions section. */
    LzReport plain = {0};
    plain.severity = "warning";
    plain.message = "m";
    plain.offset = 1;
    plain.length = 1;
    LZ_CHECK(report_is("\tx\n", &plain,
                       "warning: m\n"
                       " --> t.lz:1:2\n"
                       "  |\n"
                       "1 |     x\n"
                       "  |     ^\n"
                       "\n"));
}

static void test_colour_codes(void) {
    LzSource source;
    lz_source_from_string(&source, "t.lz", "x\n");
    LzReport report = {0};
    report.severity = "error";
    report.message = "m";
    report.length = 1;
    LzBuffer coloured = {0};
    LzWriter writer = lz_writer_from_buffer(&coloured);
    lz_report_write(&source, &report, true, &writer);
    LZ_CHECK(coloured.data != NULL && strstr(coloured.data, "\x1b[") != NULL);
    lz_buffer_free(&coloured);

    LzBuffer plain = {0};
    writer = lz_writer_from_buffer(&plain);
    lz_report_write(&source, &report, false, &writer);
    LZ_CHECK(plain.data != NULL && strchr(plain.data, '\x1b') == NULL);
    lz_buffer_free(&plain);
}

int main(void) {
    test_edit_distance();
    test_ranking();
    test_builtin_hint();
    test_script_functions_and_locals();
    test_compile_error_suggestions();
    test_report_layout();
    test_colour_codes();
    return LZ_TEST_RESULT();
}
