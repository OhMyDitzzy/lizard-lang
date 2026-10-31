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



/* The program must not compile; the first error has this text and is on this line. */
static void check_compile_error(const char *text, const char *message, uint32_t line) {
    Program program;
    run(&program, text, 0, 0);
    bool ok = program.compile_status == LZ_ERR_COMPILE && program.diagnostics.count > 0 &&
              strstr(program.diagnostics.items[0].message, message) != NULL;
    if (!ok) {
        fprintf(stderr, "program:\n%s\nexpected a compile error with [%s]; got status %d [%s]\n", text,
                message, (int)program.compile_status,
                program.diagnostics.count > 0 ? program.diagnostics.items[0].message : "no diagnostic");
    }
    LZ_CHECK(ok);
    if (ok && line != 0) {
        LzLocation at = lz_source_locate(&program.source, program.diagnostics.items[0].offset);
        if (at.line != line) {
            fprintf(stderr, "error at line %u, expected %u\n", (unsigned)at.line, (unsigned)line);
        }
        LZ_CHECK(at.line == line);
    }
    finish(&program);
}

static void test_raw_and_fences(void) {
    check_output("println(r\"C:\\data\\new\\n\")\n", "C:\\data\\new\\n\n");
    check_output("println(\"a\\tb\", r\"a\\tb\")\n", "a\tb a\\tb\n");
    check_output("println(\"\" == \"\", $\"\" == \"\", r\"\" == \"\")\n", "true true true\n");
    /* A long fence on one line may hold quotes. */
    check_output("println(\"\"\"say \"hi\" now\"\"\")\n", "say \"hi\" now\n");
    check_output("println(r\"\"\"C:\\x\\\"q\" \"\"\")\n", "C:\\x\\\"q\" \n");
    check_output("println(\"\"\"\"a \"\"\" b\"\"\"\")\n", "a \"\"\" b\n");
    check_output("println('it\\'s', 'say \"x\"')\n", "it's say \"x\"\n");
    check_output("println('''a 'b' c''')\n", "a 'b' c\n");
    check_compile_error("println(r\"abc\n", "unterminated string", 1);
    check_compile_error("println(\"\"\"a \"\"\"\" b\"\"\")\n", "ambiguous quotes", 1);
}

static void test_multiline(void) {
    check_output("println(\"\"\"\n  a\n  b\n  \"\"\")\n", "a\nb\n");
    /* Indentation of the closing quotes goes; the rest of a line's indentation stays. */
    check_output("println(\"\"\"\n    x\n      y\n    \"\"\")\n", "x\n  y\n");
    /* The line before the closing quotes decides the final line break. */
    check_output("println(\"\"\"\n  a\n\n  \"\"\" + \"|\")\n", "a\n|\n");
    check_output("println(\"\"\"\n\"\"\" == \"\")\n", "true\n");
    /* Blank lines need no indentation. */
    check_output("println(\"\"\"\n    a\n\n    b\n    \"\"\")\n", "a\n\nb\n");
    /* CRLF is one line break. */
    check_output("println(\"\"\"\r\n  a\r\n  b\r\n  \"\"\")\n", "a\nb\n");
    /* A backslash at the end of a line joins it with the next. */
    check_output("println(\"\"\"\n  a \\\n  b\n  \"\"\")\n", "a b\n");
    check_output("println(r\"\"\"\n  a \\\n  b\n  \"\"\")\n", "a \\\nb\n");
    /* Quotes inside are plain text; only the closing line ends the string. */
    check_output("println(\"\"\"\n  say \"\"\" now\n  \"\"\")\n", "say \"\"\" now\n");
    check_output("println(\"\"\"\"\n  has \"\"\"\n  \"\"\"\")\n", "has \"\"\"\n");
    /* Code may follow the closing quotes. */
    check_output("println(len(\"\"\"\n  ab\n  \"\"\"))\n", "2\n");
    check_output("let x = [\"\"\"\n  a\n  \"\"\", \"b\"]\nprintln(x)\n", "[\"a\", \"b\"]\n");

    check_compile_error("println(\"\"\"\n    a\n  b\n    \"\"\")\n", "indented less", 3);
    check_compile_error("println(\"\"\"\n    a\n\t\"\"\")\n", "does not match", 2);
    check_compile_error("println(\"\"\"\n  a\n", "unterminated multi-line", 1);
    check_compile_error("println(\"\"\"\n  a  \"\"\")\n", "unterminated multi-line", 1);
}

static void test_templates(void) {
    check_output("let n = \"Ana\"\nprintln($\"hi {n}!\")\n", "hi Ana!\n");
    check_output("println($\"{1}{2}{3}\", $\"{1 + 2}\", $\"{true} {nil}\")\n", "123 3 true nil\n");
    check_output("println($\"{[1, 2]} {[\"a\"]}\")\n", "[1, 2] [\"a\"]\n");
    /* Braces. */
    check_output("println($\"{{x}} {{\", $\"}}\")\n", "{x} { }\n");
    check_output("println(\"{x} {{\")\n", "{x} {{\n");
    check_output("let a = 1\nprintln($r\"{a}\\n{{\")\n", "1\\n{\n");
    /* Any expression, strings and templates inside. */
    check_output("println($\"{\"a\" + \"b\"} {len(\"xyz\")}\")\n", "ab 3\n");
    check_output("let u = \"x\"\nprintln($\"<{$\"[{u}]\"}>\")\n", "<[x]>\n");
    check_output("println($\"{[1, 2].map(fn(x) => x * 2)}\")\n", "[2, 4]\n");
    check_output("println($\"{[1, 2][1]} {fn(a: int) => a}\")\n", "2 <fn>\n");
    /* Left to right, once each. */
    check_output("let c = 0\nfn tick() {\n c = c + 1\n return c\n}\nprintln($\"{tick()}{tick()}{tick()}\", c)\n",
                 "123 3\n");
    /* A user function named like a method cannot hijack a template. */
    check_output("fn to_string(x) { return \"hijacked\" }\nprintln($\"{1}\")\n", "1\n");
    /* Multi-line templates; expressions may span lines. */
    check_output("let n = 2\nprintln($\"\"\"\n  a {n}\n    b {\n      n + 1\n    }\n  \"\"\")\n",
                 "a 2\n  b 3\n");
    check_output("let n = 2\nprintln($r\"\"\"\n  a\\n {n}\n  \"\"\")\n", "a\\n 2\n");
    check_output("let n = 7\nprintln($\"{n}\\n{n}\")\n", "7\n7\n");

    check_compile_error("println($\"a } b\")\n", "single '}'", 1);
    check_compile_error("println($\"a {} b\")\n", "empty '{}'", 1);
    check_compile_error("println($\"a { } b\")\n", "empty '{}'", 1);
    check_compile_error("println($\"a {n\")\n", "unclosed '{'", 1);
    check_compile_error("println($\"a {n # c}\")\n", "comment", 1);
    check_compile_error("println($\"a \\{\")\n", "write '{{'", 1);
    check_compile_error("println($\"{1 +}\")\n", "expected expression", 1);
    check_compile_error("println($\"{1 2}\")\n", "unexpected token", 1);
    check_compile_error("let t: $\"a\" = 1\n", "cannot be used as a type", 1);
    check_compile_error("println($x)\n", "'$' must come directly", 1);
    check_compile_error("println($\"\"\"\n a {\n\n", "unclosed '{'", 2);
}

static void test_format_specs(void) {
    check_output("let n = 5\nprintln($\"[{n:>5}] [{n:<5}] [{n:^5}] [{n:*^7}] [{n:05}] [{n:+}]\")\n",
                 "[    5] [5    ] [  5  ] [***5***] [00005] [+5]\n");
    check_output("println($\"[{\"ab\":^5}] [{\"ab\":>5}] [{\"ab\":-<5}] [{\"abcdef\":.3}]\")\n",
                 "[ ab  ] [   ab] [ab---] [abc]\n");
    check_output("println($\"{3.14159:.2f} {2:.1f} {1234567.891:,.2f} {0.256:.1%} {1500:.2e}\")\n",
                 "3.14 2.0 1,234,567.89 25.6% 1.50e+03\n");
    check_output("println($\"{255:#x} {255:#06x} {255:X} {255:o} {255:#b} {-255:x}\")\n",
                 "0xff 0x00ff FF 377 0b11111111 -ff\n");
    check_output("println($\"{1000000:,} {1000000:_} {255:_b} {-1234567:,}\")\n",
                 "1,000,000 1_000_000 1111_1111 -1,234,567\n");
    check_output("println($\"{65:c} {128512:c} {7:03d} {-7:04d} {-3.5:08.2f} {2.5:+.1f} {3:.0f}\")\n",
                 "A \xF0\x9F\x98\x80 007 -007 -0003.50 +2.5 3\n");
    check_output("println($\"{1.5} {2.5:6} {10:6} {true:6}|{nil:<5}|\")\n",
                 "1.5    2.5     10 true  |nil  |\n");
    check_output("println($\"{\"\xC3\xA9\xC3\xA9\":*>4}\")\n", "**\xC3\xA9\xC3\xA9\n");
    /* Dynamic widths and precisions. */
    check_output("let w = 6\nlet p = 1\nprintln($\"[{\"ab\":>{w}}] [{3.14159:{w}.{p}f}] [{7:0{w}}]\")\n",
                 "[    ab] [   3.1] [000007]\n");
    /* format() is the same engine, with a spec built at run time. */
    check_output("println((255).format(\"#x\"), \"ab\".format(\">4\") + \"|\", (3.14159).format(\".3f\"))\n",
                 "0xff   ab| 3.142\n");
    /* The debug form. */
    check_output("let name = \"Ana\"\nlet n = 5\nprintln($\"{name=} {n=} {n + 1=} {n = }\")\n",
                 "name=\"Ana\" n=5 n + 1=6 n = 5\n");
    check_output("let n = 5\nprintln($\"{n=:>4}|{n == 5=}|{n >= 5}\")\n", "n=   5|n == 5=true|true\n");
    check_output("println(\"a\\n\".to_debug(), [1, \"b\"].to_debug(), (2.5).to_debug())\n",
                 "\"a\\n\" [1, \"b\"] 2.5\n");

    check_compile_error("println($\"{5:.2d}\")\n", "precision does not apply", 1);
    check_compile_error("println($\"{5:q}\")\n", "unknown format type 'q'", 1);
    check_compile_error("println($\"{5:>05}\")\n", "'0' cannot be combined", 1);
    check_compile_error("println($\"{5:,x}\")\n", "decimal digits", 1);
    check_compile_error("println($\"{5:#d}\")\n", "'#' only works", 1);
    check_compile_error("println($\"{5:>>>}\")\n", "unknown format type", 1);
    check_compile_error("println($\"{5:.}\")\n", "digits after '.'", 1);
    check_compile_error("println($\"{5:999999}\")\n", "too large", 1);
    check_compile_error("println($\"{5:{3:4}}\")\n", "cannot have its own spec", 1);
    check_fails("println($\"{\"x\":+}\")\n", LZ_ERR_RUNTIME, "sign only works with numbers", 1);
    check_fails("println($\"{2.5:d}\")\n", LZ_ERR_RUNTIME, "needs an int", 1);
    check_fails("println($\"{\"x\":f}\")\n", LZ_ERR_RUNTIME, "needs a number", 1);
    check_fails("println($\"{5:.2}\")\n", LZ_ERR_RUNTIME, "precision does not apply to an int", 1);
    check_fails("println($\"{-1:c}\")\n", LZ_ERR_RUNTIME, "not a valid code point", 1);
    check_fails("let w = \"q\"\nprintln($\"{5:{w}}\")\n", LZ_ERR_RUNTIME, "invalid spec", 2);
}

int main(void) {
    test_raw_and_fences();
    test_multiline();
    test_templates();
    test_format_specs();
    return LZ_TEST_RESULT();
}
