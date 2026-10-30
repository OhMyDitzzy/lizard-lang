#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "lizard/array.h"
#include "lizard/builtin.h"
#include "lizard/compiler.h"
#include "lizard/debug.h"
#include "test_util.h"

typedef struct Program {
    LzVM vm;
    LzBuffer out;
    LzBuffer err;
    LzChunk chunk;
    LzDiagnostics diagnostics;
    LzSource source;
    LzStatus compile_status;
    LzStatus run_status;
} Program;

/* Fuel and heap limit of 0 mean unlimited. */
static void run(Program *program, const char *text, uint64_t fuel, size_t heap_limit) {
    memset(program, 0, sizeof *program);
    lz_vm_init(&program->vm, lz_writer_from_buffer(&program->out),
               lz_writer_from_buffer(&program->err));
    LZ_CHECK(lz_builtin_install(&program->vm) == LZ_OK);

    lz_source_from_string(&program->source, "t.lz", text);
    lz_chunk_init(&program->chunk);
    program->compile_status =
        lz_compile(&program->vm, &program->source, &program->chunk, &program->diagnostics);

    if (fuel != 0) {
        lz_vm_set_fuel(&program->vm, fuel);
    }
    if (heap_limit != 0) {
        lz_vm_set_heap_limit(&program->vm, heap_limit);
    }
    if (program->compile_status == LZ_OK) {
        program->run_status = lz_vm_run(&program->vm, &program->chunk);
    }
}

static void finish(Program *program) {
    LZ_CHECK(program->vm.stack_count == 0);
    lz_chunk_free(&program->vm.heap, &program->chunk);
    lz_vm_release_roots(&program->vm);
    LZ_CHECK(program->vm.heap.live_objects == 0); /* no leaked array, however it ended */
    LZ_CHECK(program->vm.heap.live_bytes == 0);   /* item buffers are accounted for and returned */
    lz_vm_free(&program->vm);
    lz_buffer_free(&program->out);
    lz_buffer_free(&program->err);
}

static void check_output(const char *text, const char *expected) {
    Program program;
    run(&program, text, 0, 0);
    LZ_CHECK(program.compile_status == LZ_OK);
    LZ_CHECK(program.run_status == LZ_OK);
    const char *actual = program.out.data != NULL ? program.out.data : "";
    bool same = strcmp(actual, expected) == 0;
    if (!same) {
        fprintf(stderr, "program:\n%s\nexpected: [%s]\ngot: [%s]\n", text, expected, actual);
        for (size_t i = 0; i < program.diagnostics.count; i++) {
            fprintf(stderr, "diagnostic: %s\n", program.diagnostics.items[i].message);
        }
        if (program.run_status != LZ_OK) {
            fprintf(stderr, "runtime: %s\n", program.vm.error);
        }
    }
    LZ_CHECK(same);
    finish(&program);
}

static void check_fails(const char *text, LzStatus status, const char *message, uint32_t line) {
    Program program;
    run(&program, text, 0, 0);
    LZ_CHECK(program.compile_status == LZ_OK);
    bool ok = program.run_status == status && strstr(program.vm.error, message) != NULL;
    if (!ok) {
        fprintf(stderr, "program:\n%s\nexpected status %d with [%s], got %d [%s]\n", text,
                (int)status, message, (int)program.run_status, program.vm.error);
    }
    LZ_CHECK(ok);
    if (line != 0) {
        LZ_CHECK(program.vm.error_offset != LZ_NO_OFFSET);
        LzLocation at = lz_source_locate(&program.source, program.vm.error_offset);
        if (at.line != line) {
            fprintf(stderr, "error at line %u, expected %u\n", (unsigned)at.line, (unsigned)line);
        }
        LZ_CHECK(at.line == line);
    }
    finish(&program);
}


static void test_text_methods(void) {
    check_output("println(\"Hello\".to_upper(), \"Hello\".to_lower())\n", "HELLO hello\n");
    check_output("println(\"  hi  \".trim() + \"|\", \"  hi  \".trim_start() + \"|\", \"  hi  \".trim_end() + \"|\")\n",
                 "hi| hi  |   hi|\n");
    check_output("println(\"a,b,,c\".split(\",\"), \"abc\".split(\"x\"), \"\".split(\",\"))\n",
                 "[\"a\", \"b\", \"\", \"c\"] [\"abc\"] [\"\"]\n");
    check_output("println(\"ab--cd\".split(\"--\"))\n", "[\"ab\", \"cd\"]\n");
    check_output("println(\"hello world\".contains(\"o w\"), \"hello\".contains(\"z\"), \"x\".contains(\"\"))\n",
                 "true false true\n");
    check_output("println(\"hello\".starts_with(\"he\"), \"hello\".starts_with(\"lo\"), \"hello\".ends_with(\"lo\"))\n",
                 "true false true\n");
    check_output("println(\"hello\".replace(\"l\", \"L\"), \"aaa\".replace(\"aa\", \"b\"), \"abc\".replace(\"b\", \"\"))\n",
                 "heLLo ba ac\n");
    check_output("println(\"ab\".repeat(3), \"ab\".repeat(0) == \"\")\n", "ababab true\n");
    check_output("println(\"hello\".index_of(\"llo\"), \"hello\".index_of(\"z\"), \"hello\".index_of(\"\"))\n",
                 "2 -1 0\n");
    /* Positions count characters, not bytes. */
    check_output("println(\"h\xC3\xA9llo\".chars(), \"h\xC3\xA9llo\".index_of(\"l\"))\n",
                 "[\"h\", \"\xC3\xA9\", \"l\", \"l\", \"o\"] 2\n");
    check_output("println(\"hello\".slice(1, 3), \"hello\".slice(-3), \"hello\".slice(0, -1), \"hello\".slice(3, 1) == \"\")\n",
                 "el llo hell true\n");
    check_output("println(\"h\xC3\xA9llo\".slice(1, 3), \"hello\".slice(nil, 2), \"hello\".slice(9))\n",
                 "\xC3\xA9l he \n");
    check_output("println(\"\".is_empty(), \"a\".is_empty())\n", "true false\n");
    check_output("println(\"12\".to_int() + 1, \"2.5\".to_float())\n", "13 2.5\n");
}

static void test_unicode_text(void) {
    check_output("println(\"h\xC3\xA9llo\".to_upper(), \"\xC3\x89" "COLE\".to_lower())\n",
                 "H\xC3\x89LLO \xC3\xA9" "cole\n");
    /* Cyrillic and Greek, and a letter whose case pair is not next to it (y with diaeresis). */
    check_output("println(\"\xD0\xBF\xD1\x80\".to_upper(), \"\xCE\x91\xCE\x92\".to_lower(), \"\xC3\xBF\".to_upper())\n",
                 "\xD0\x9F\xD0\xA0 \xCE\xB1\xCE\xB2 \xC5\xB8\n");
    /* Simple mapping: sharp s has no single-letter capital, so it stays. */
    check_output("println(\"stra\xC3\x9F" "e\".to_upper())\n", "STRA\xC3\x9F" "E\n");
    /* Letters without case, and characters outside the BMP, are kept. */
    check_output("println(\"\xE6\x97\xA5\xE6\x9C\xAC abc\".to_upper(), \"\xF0\x9F\x98\x80 ok\".to_upper())\n",
                 "\xE6\x97\xA5\xE6\x9C\xAC ABC \xF0\x9F\x98\x80 OK\n");
    /* Unicode white space is trimmed: no-break space and ideographic space. */
    check_output("println(\"[\" + \"\xC2\xA0 x\xE3\x80\x80\".trim() + \"]\")\n", "[x]\n");
    check_output("println(\"[\" + \"\xE3\x80\x80x\xE3\x80\x80\".trim_end() + \"]\")\n", "[\xE3\x80\x80x]\n");
    /* A letter that merely ends in the same bytes as a space is not trimmed (U+00E0 is C3 A0). */
    check_output("println(\"x\xC3\xA0\".trim().len())\n", "2\n");
    /* Bytes that are not UTF-8 pass through. */
    Program program;
    run(&program, "let s = \"a\xFF" "b\"\nprintln(len(s.to_upper()), len(s.trim()))\n", 0, 0);
    LZ_CHECK(program.compile_status != LZ_OK || program.run_status == LZ_OK);
    finish(&program);
}

static void test_number_methods(void) {
    check_output("println((-5).abs(), (-2.5).abs(), 3.abs())\n", "5 2.5 3\n");
    check_output("println(2.7.floor(), (-2.7).floor(), 2.2.ceil(), 2.5.round(), (-2.5).round(), 7.floor())\n",
                 "2 -3 3 3 -3 7\n");
    check_output("println(16.sqrt(), 2.pow(10), 2.pow(-1), 2.5.pow(2), 0.pow(0))\n", "4.0 1024 0.5 6.25 1\n");
    check_output("println(3.min(5), 3.max(5), 3.max(5.5), 3.min(2.5), 2.max(2.0))\n", "3 5 5.5 2.5 2\n");
    check_fails("println(2.pow(64))\n", LZ_ERR_RUNTIME, "integer overflow", 0);
    check_fails("println((-1).sqrt())\n", LZ_ERR_RUNTIME, "negative number", 0);
    check_fails("println(1e300.round())\n", LZ_ERR_RUNTIME, "out of range", 0);
    check_fails("println(3.min(\"a\"))\n", LZ_ERR_RUNTIME, "expected a number", 0);
}

static void test_universal_and_array_methods(void) {
    check_output("println(42.to_string(), 3.5.to_string(), true.to_string(), nil.to_string())\n",
                 "42 3.5 true nil\n");
    check_output("println([1, \"a\"].to_string(), 0.to_bool(), nil.to_bool())\n", "[1, \"a\"] true false\n");
    check_output("println(5.to_string().len(), 5.to_string() + \"!\")\n", "1 5!\n");
    check_output("println([1, 2].first(), [1, 2].last(), [].first(), [].is_empty(), [1].is_empty())\n",
                 "1 2 nil true false\n");
    /* A kind without a method of that name uses the function of the same name. */
    check_output("let xs = [3, 4]\nprintln(xs.contains(4), xs.index_of(4), xs.min(), xs.max(), xs.len())\n",
                 "true 1 3 4 2\n");
    check_output("println(\"a b\".split(\" \").map(fn(s) => s.to_upper()))\n", "[\"A\", \"B\"]\n");
}

static void test_method_resolution(void) {
    /* Your own function or variable of that name wins over the method. */
    check_output("fn trim(s) { return \"mine\" }\nprintln(\" x \".trim())\n", "mine\n");
    check_fails("let trim = 5\nprintln(\" x \".trim())\n", LZ_ERR_RUNTIME, "can only call functions", 0);
    /* Named arguments and a wrong kind. */
    check_fails("println(5.to_lower())\n", LZ_ERR_RUNTIME, "int has no method 'to_lower' (it works on string)", 0);
    check_fails("println(nil.trim())\n", LZ_ERR_RUNTIME, "nil has no method 'trim'", 0);
    check_fails("println(\"abc\".trim(1))\n", LZ_ERR_ARITY, "trim(): expected 0 arguments, got 1", 0);
    check_fails("println(\"abc\".slice())\n", LZ_ERR_ARITY, "slice(): expected 1 to 2 arguments, got 0", 0);
    check_fails("println(\"abc\".split(\"\"))\n", LZ_ERR_RUNTIME, "separator cannot be empty", 0);
    check_fails("println(\"abc\".replace(\"\", \"x\"))\n", LZ_ERR_RUNTIME, "cannot be empty", 0);
    check_fails("println(\"abc\".repeat(-1))\n", LZ_ERR_RUNTIME, "cannot be negative", 0);
    check_fails("println(\"abc\".repeat(1000000000))\n", LZ_ERR_RUNTIME, "result too long", 0);
    check_fails("println(\"a\".contains(5))\n", LZ_ERR_RUNTIME, "expected a string, got int", 0);

    /* A typo suggests the method names too. */
    Program program;
    run(&program, "println(\"abc\".to_lowr())\n", 0, 0);
    LZ_CHECK(program.run_status == LZ_ERR_RUNTIME);
    const LzNameHint *hint = lz_diagnostics_find_hint(&program.diagnostics, program.vm.error_offset);
    LZ_CHECK(hint != NULL && hint->is_method);
    if (hint != NULL) {
        LZ_CHECK(hint->suggestions.count >= 1 && strcmp(hint->suggestions.names[0], "to_lower") == 0);
    }
    finish(&program);
}

static void test_fuel_and_heap(void) {
    Program program;
    run(&program, "let s = \"x\".repeat(100000)\nprintln(len(s.to_upper()))\n", 1000, 0);
    LZ_CHECK(program.run_status == LZ_ERR_FUEL);
    finish(&program);

    run(&program, "println(\"x\".repeat(1000000).len())\n", 0, 100000);
    LZ_CHECK(program.run_status != LZ_OK); /* the heap limit stops it */
    finish(&program);
}

int main(void) {
    test_text_methods();
    test_unicode_text();
    test_number_methods();
    test_universal_and_array_methods();
    test_method_resolution();
    test_fuel_and_heap();
    return LZ_TEST_RESULT();
}
