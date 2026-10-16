#include <stdio.h>
#include <string.h>

#include "lizard/source.h"
#include "test_util.h"

static void test_locate(void) {
    LzSource source;
    lz_source_from_string(&source, "t.lz", "ab\ncd\n");

    LzLocation at = lz_source_locate(&source, 0);
    LZ_CHECK(at.line == 1 && at.column == 1);
    at = lz_source_locate(&source, 1);
    LZ_CHECK(at.line == 1 && at.column == 2);
    at = lz_source_locate(&source, 3);
    LZ_CHECK(at.line == 2 && at.column == 1 && at.line_start == 3 && at.line_end == 5);
    at = lz_source_locate(&source, 4);
    LZ_CHECK(at.line == 2 && at.column == 2);
    at = lz_source_locate(&source, 6);
    LZ_CHECK(at.line == 3 && at.column == 1 && at.line_start == 6 && at.line_end == 6);
    at = lz_source_locate(&source, 999);
    LZ_CHECK(at.line == 3);

    lz_source_from_string(&source, "t.lz", "");
    at = lz_source_locate(&source, 0);
    LZ_CHECK(at.line == 1 && at.column == 1 && at.line_end == 0);

    lz_source_from_string(&source, "t.lz", "\xC3\xA9=1");
    at = lz_source_locate(&source, 2);
    LZ_CHECK(at.line == 1 && at.column == 2);

    lz_source_from_string(&source, "t.lz", "ab\r\ncd");
    at = lz_source_locate(&source, 0);
    LZ_CHECK(at.line_end == 2);
    at = lz_source_locate(&source, 4);
    LZ_CHECK(at.line == 2 && at.column == 1);
}

static bool diagnostic_is(const char *text, size_t offset, size_t length, const char *severity,
                          const char *expected) {
    LzSource source;
    lz_source_from_string(&source, "t.lz", text);

    LzBuffer buffer = {0};
    LzWriter writer = lz_writer_from_buffer(&buffer);
    lz_source_write_diagnostic(&source, offset, length, severity, "m", &writer);

    bool same = buffer.data != NULL && strcmp(buffer.data, expected) == 0;
    if (!same && buffer.data != NULL) {
        fprintf(stderr, "got:\n%s\nexpected:\n%s\n", buffer.data, expected);
    }
    lz_buffer_free(&buffer);
    return same;
}

static void test_diagnostics(void) {
    LZ_CHECK(diagnostic_is("let x = @\n", 8, 1, "error",
                           "t.lz:1:9: error: m\n"
                           " 1 | let x = @\n"
                           "   |         ^\n"));

    LZ_CHECK(diagnostic_is("\ty = 1 +\n", 7, 1, "error",
                           "t.lz:1:8: error: m\n"
                           " 1 | \ty = 1 +\n"
                           "   | \t      ^\n"));

    LZ_CHECK(diagnostic_is("a\nbcd\ne", 3, 5, "warning",
                           "t.lz:2:2: warning: m\n"
                           " 2 | bcd\n"
                           "   |  ^^\n"));

    LZ_CHECK(diagnostic_is("ab", 2, 0, "error",
                           "t.lz:1:3: error: m\n"
                           " 1 | ab\n"
                           "   |   ^\n"));

    LZ_CHECK(diagnostic_is("\xC3\xA9+1", 0, 2, "error",
                           "t.lz:1:1: error: m\n"
                           " 1 | \xC3\xA9+1\n"
                           "   | ^\n"));

    LZ_CHECK(diagnostic_is("a\r\nb", 0, 1, "error",
                           "t.lz:1:1: error: m\n"
                           " 1 | a\n"
                           "   | ^\n"));
}

static void test_long_names_never_cut_a_diagnostic(void) {
    char name[401];
    memset(name, 'd', 400);
    name[400] = '\0';

    LzSource source;
    lz_source_from_string(&source, name, "let x = @\n");
    LzBuffer buffer = {0};
    LzWriter writer = lz_writer_from_buffer(&buffer);
    lz_source_write_diagnostic(&source, 8, 1, "error", "a message that must survive", &writer);

    LZ_CHECK(buffer.data != NULL);
    if (buffer.data != NULL) {
        LZ_CHECK(strncmp(buffer.data, name, 400) == 0);
        LZ_CHECK(strcmp(buffer.data + 400, ":1:9: error: a message that must survive\n"
                                           " 1 | let x = @\n"
                                           "   |         ^\n") == 0);
    }
    lz_buffer_free(&buffer);
}

static void test_read_file(void) {
    const char *path = "lz_test_source.tmp";
    FILE *file = fopen(path, "wb");
    LZ_CHECK(file != NULL);
    if (file == NULL) {
        return;
    }
    (void)fputs("x = 1\ny\n", file);
    (void)fclose(file);

    LzSource source;
    char error[200] = "";
    LZ_CHECK(lz_source_read_file(&source, path, error, sizeof error));
    LZ_CHECK(source.length == 8);
    LZ_CHECK(source.owned);
    LZ_CHECK(strcmp(source.text, "x = 1\ny\n") == 0);
    LZ_CHECK(strcmp(source.name, path) == 0);
    lz_source_free(&source);
    LZ_CHECK(source.text == NULL);
    (void)remove(path);

    LZ_CHECK(!lz_source_read_file(&source, "lz_no_such_file.lz", error, sizeof error));
    LZ_CHECK(strstr(error, "cannot open") != NULL);

    file = fopen(path, "wb");
    if (file != NULL) {
        (void)fclose(file);
        LZ_CHECK(lz_source_read_file(&source, path, error, sizeof error));
        LZ_CHECK(source.length == 0 && source.text[0] == '\0');
        lz_source_free(&source);
        (void)remove(path);
    }
}

static void write_file(const char *path, const char *bytes, size_t length) {
    FILE *file = fopen(path, "wb");
    LZ_CHECK(file != NULL);
    if (file != NULL) {
        LZ_CHECK(fwrite(bytes, 1, length, file) == length);
        (void)fclose(file);
    }
}

static void test_byte_order_mark(void) {
    const char *path = "lz_test_bom.tmp";
    LzSource source;
    char error[200] = "";

    write_file(path, "\xEF\xBB\xBFx = 1\n", 9);
    LZ_CHECK(lz_source_read_file(&source, path, error, sizeof error));
    LZ_CHECK(source.length == 6);
    LZ_CHECK(strcmp(source.text, "x = 1\n") == 0);
    lz_source_free(&source);

    write_file(path, "\xEF\xBB\xBF", 3);
    LZ_CHECK(lz_source_read_file(&source, path, error, sizeof error));
    LZ_CHECK(source.length == 0 && source.text[0] == '\0');
    lz_source_free(&source);

    /* Only a complete mark at the very start is dropped. */
    write_file(path, "\xEF\xBBx", 3);
    LZ_CHECK(lz_source_read_file(&source, path, error, sizeof error));
    LZ_CHECK(source.length == 3);
    lz_source_free(&source);
    (void)remove(path);
}

/* A file of exactly LZ_SOURCE_MAX_BYTES is accepted; one more byte is too much. */
static void test_size_limit(void) {
    const char *path = "lz_test_limit.tmp";
    FILE *file = fopen(path, "wb");
    LZ_CHECK(file != NULL);
    if (file == NULL) {
        return;
    }
    LZ_CHECK(fseek(file, (long)LZ_SOURCE_MAX_BYTES - 1, SEEK_SET) == 0);
    LZ_CHECK(fputc('\n', file) != EOF);
    (void)fclose(file);

    LzSource source;
    char error[200] = "";
    LZ_CHECK(lz_source_read_file(&source, path, error, sizeof error));
    LZ_CHECK(source.length == LZ_SOURCE_MAX_BYTES);
    lz_source_free(&source);

    file = fopen(path, "ab");
    LZ_CHECK(file != NULL);
    if (file != NULL) {
        LZ_CHECK(fputc('\n', file) != EOF);
        (void)fclose(file);
        LZ_CHECK(!lz_source_read_file(&source, path, error, sizeof error));
        LZ_CHECK(strstr(error, "larger than") != NULL);
    }
    (void)remove(path);
}

int main(void) {
    test_locate();
    test_diagnostics();
    test_long_names_never_cut_a_diagnostic();
    test_read_file();
    test_byte_order_mark();
    test_size_limit();
    return LZ_TEST_RESULT();
}
