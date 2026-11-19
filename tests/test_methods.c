#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "lizard/array.h"
#include "lizard/builtin.h"
#include "lizard/compiler.h"
#include "lizard/class.h"
#include "lizard/debug.h"
#include "method/method.h"
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
    check_output("println(\",a,\".split(\",\"), \"--a----\".split(\"--\"), \"\".split(\",\"))\n"
                 "println(\"a\".contains(\"a\"), \"abc\".contains(\"abc\"), \"abcabc\".split(\"abc\"))\n",
                 "[\"\", \"a\", \"\"] [\"\", \"a\", \"\", \"\"] [\"\"]\ntrue true [\"\", \"\", \"\"]\n");
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
    check_output("println([1, \"a\"].to_string(), 0.to_bool(), nil.to_bool())\n", "[1, \"a\"] false false\n");
    check_output("println(5.to_string().len(), 5.to_string() + \"!\")\n", "1 5!\n");
    check_output("println([1, 2].first(), [1, 2].last(), [].first(), [].is_empty(), [1].is_empty())\n",
                 "1 2 nil true false\n");
    /* A kind without a method of that name uses the function of the same name. */
    check_output("let xs = [3, 4]\nprintln(xs.contains(4), xs.index_of(4), xs.min(), xs.max(), xs.len())\n",
                 "true 1 3 4 2\n");
    check_output("println(\"a b\".split(\" \").map(fn(s) => s.to_upper()))\n", "[\"A\", \"B\"]\n");
}

static void check_compile_error(const char *text, const char *message) {
    Program program;
    run(&program, text, 0, 0);
    LZ_CHECK(program.compile_status == LZ_ERR_COMPILE);
    bool found = false;
    for (size_t i = 0; i < program.diagnostics.count; i++) {
        found = found || strstr(program.diagnostics.items[i].message, message) != NULL;
    }
    if (!found) {
        fprintf(stderr, "program:\n%s\nexpected an error containing [%s]\n", text, message);
    }
    LZ_CHECK(found);
    finish(&program);
}

/* A function is not a method: `value.name()` never falls back to `name(value)`. */
static void test_no_ufcs(void) {
    check_compile_error("fn x(z: string) { return z }\nprintln(\"\".x())\n", "'x' is a function, not a method of string");
    check_compile_error("fn double(n) { return n * 2 }\nprintln(5.double())\n", "'double' is a function, not a method of int");
    check_compile_error("println(5.println())\n", "'println' is a function, not a method of int");
    check_compile_error("println(5.typeof())\n", "'typeof' is a function, not a method of int");
    check_compile_error("println(\"a\".sum())\n", "'sum' is a function, not a method of string");
    /* A receiver whose type is only known at run time fails when it runs. */
    check_fails("fn shout(s) { return s + \"!\" }\nfn run(v) { return v.shout() }\nprintln(run(\"a\"))\n", LZ_ERR_RUNTIME,
                "'shout' is a function, not a method of string", 0);
    check_fails("class P { pub x: int }\nfn helper(p) { return 1 }\nprintln(P(1).helper())\n", LZ_ERR_RUNTIME,
                "'helper' is a function, not a method of P", 0);
    /* An unknown name keeps its own message. */
    check_fails("println(5.to_lower())\n", LZ_ERR_RUNTIME, "int has no method 'to_lower' (it works on string)", 0);
    /* What stays: real methods, the global functions, and `extend`. */
    check_output("let xs = [3, 1, 2]\nxs.push(4)\nxs.sort()\nprintln(xs, xs.len(), xs.sum())\npush(xs, 5)\nprintln(xs)\n", "[1, 2, 3, 4] 4 10\n[1, 2, 3, 4, 5]\n");
    check_output("println([1, 2, 3].map(fn(n) => n * 2).filter(fn(n) => n > 2).join(\"-\"), \"abc\".len())\n", "4-6 3\n");
    check_output("fn trim(s) => \"mine\"\nextend string { pub fn shout(self) => self.to_upper() + \"!\" }\nprintln(\" a \".trim(), \"hi\".shout())\n", "a HI!\n");
    check_output("fn sq(x) => x * x\nclass Box { pub cb = nil }\nprintln(Box(sq).cb(3))\n", "9\n");
}

static void test_typeof(void) {
    check_output("println(typeof(\"s\"), typeof(1), typeof(1.5), typeof(true), typeof(nil))\n", "string int float bool nil\n");
    check_output("fn f() => 1\nprintln(typeof([1]), typeof({\"a\": 1}), typeof(f), typeof(fn(x) => x), typeof(println))\n", "array dict function function function\n");
    check_output("println(typeof(Error(\"x\")), typeof(ValueError(\"x\")))\n", "error error\n");
    check_output("class Dog { pub n: string }\nlet d = Dog(\"a\")\nprintln(typeof(d), typeof(link(d)), typeof(link(d).target))\n", "Dog link Dog\n");
    check_output("error Overdraft : ValueError { pub needed: int }\nprintln(typeof(Overdraft(\"m\", needed: 5)))\n", "Overdraft\n");
    check_output("println(typeof([\"a\"]) == \"array\", [1] is int[], typeof(typeof(1)))\n", "true true string\n");
}

static void test_method_resolution(void) {
    /* A method is found in the type of the value: your own function or variable of that name
     * does not replace it, and it is never a method. */
    check_output("fn trim(s) { return \"mine\" }\nprintln(\" x \".trim(), trim(\" x \"))\n", "x mine\n");
    check_output("let trim = 5\nprintln(\" x \".trim())\n", "x\n");
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

static uint32_t text_random(uint32_t *state) {
    *state = *state * 1664525u + 1013904223u;
    return *state;
}

static void check_case_bytes(LzVM *vm, const char *bytes, size_t length,
                             const LzBuffer *upper, const LzBuffer *lower) {
    LzString *input = lz_string_new(&vm->heap, bytes, length);
    LZ_CHECK(input != NULL);
    if (input == NULL) return;
    LzValue arg = lz_value_object(&input->base);
    LzNativeFn functions[] = {lz_method_to_upper, lz_method_to_lower};
    const LzBuffer *expected[] = {upper, lower};
    for (size_t i = 0; i < 2; i++) {
        LzValue result = lz_value_nil();
        size_t before = vm->heap.total_allocations;
        LZ_CHECK(functions[i](vm, 1, &arg, &result) == LZ_OK);
        LZ_CHECK(result.type == LZ_VAL_OBJECT && result.as.object->type == LZ_OBJ_STRING);
        if (result.type == LZ_VAL_OBJECT && result.as.object->type == LZ_OBJ_STRING) {
            const LzString *got = (const LzString *)result.as.object;
            LZ_CHECK(got != input);
            LZ_CHECK(got->length == expected[i]->length);
            if (got->length == expected[i]->length) {
                LZ_CHECK(got->length == 0 || memcmp(got->data, expected[i]->data, got->length) == 0);
            }
            LZ_CHECK(lz_string_hash(got) == lz_hash_bytes(got->data, got->length));
            LZ_CHECK(vm->heap.total_allocations == before + 1);
        }
        lz_value_release(&vm->heap, &result);
    }
    lz_value_release(&vm->heap, &arg);
}

static void test_case_corpus(void) {
    LzVM vm;
    lz_vm_init(&vm, (LzWriter){0}, (LzWriter){0});
    LzBuffer input = {0}, upper = {0}, lower = {0};
    LzWriter in = lz_writer_from_buffer(&input), up = lz_writer_from_buffer(&upper);
    LzWriter low = lz_writer_from_buffer(&lower);
    /* Include every ASCII byte, including NUL, before generating mixed text. */
    for (unsigned c = 0; c < 128; c++) {
        char a = (char)c;
        char b = (char)(c >= 'a' && c <= 'z' ? c - 32 : c);
        char d = (char)(c >= 'A' && c <= 'Z' ? c + 32 : c);
        LZ_CHECK(lz_writer_write(&in, &a, 1));
        LZ_CHECK(lz_writer_write(&up, &b, 1));
        LZ_CHECK(lz_writer_write(&low, &d, 1));
    }
    check_case_bytes(&vm, input.data, input.length, &upper, &lower);
    const char *units[][3] = {
        {"aZ09!", "AZ09!", "az09!"},
        {"\xC3\xA9", "\xC3\x89", "\xC3\xA9"},
        {"\xCE\xA3", "\xCE\xA3", "\xCF\x83"},
        {"\xD0\xAF", "\xD0\xAF", "\xD1\x8F"},
        {"\xE1\xBA\x9E", "\xE1\xBA\x9E", "\xC3\x9F"},
        {"\xF0\x90\x90\x80", "\xF0\x90\x90\x80", "\xF0\x90\x90\xA8"},
        {"\xFF", "\xFF", "\xFF"},
        {"\xC0\xAF", "\xC0\xAF", "\xC0\xAF"},
    };
    uint32_t random = 0x5EED1234u;
    for (size_t round = 0; round < 128; round++) {
        input.length = upper.length = lower.length = 0;
        size_t count = text_random(&random) % 1024;
        for (size_t i = 0; i < count; i++) {
            size_t k = round % 3 == 0 ? 0 : text_random(&random) % 8;
            LZ_CHECK(lz_writer_write_cstr(&in, units[k][0]));
            LZ_CHECK(lz_writer_write_cstr(&up, units[k][1]));
            LZ_CHECK(lz_writer_write_cstr(&low, units[k][2]));
        }
        check_case_bytes(&vm, input.data, input.length, &upper, &lower);
    }
    input.length = upper.length = lower.length = 0;
    for (size_t i = 0; i < 256 * 1024; i++) {
        LZ_CHECK(lz_writer_write_cstr(&in, "q"));
        LZ_CHECK(lz_writer_write_cstr(&up, "Q"));
        LZ_CHECK(lz_writer_write_cstr(&low, "q"));
    }
    check_case_bytes(&vm, input.data, input.length, &upper, &lower);
    LzString *text = lz_string_new(&vm.heap, "UNCHANGED", 9);
    LzValue arg = lz_value_object(&text->base), result = lz_value_nil();
    size_t bytes = sizeof(LzString) + 10;
    lz_vm_set_heap_limit(&vm, vm.heap.live_bytes + bytes - 1);
    LZ_CHECK(lz_method_to_upper(&vm, 1, &arg, &result) == LZ_ERR_HEAP_LIMIT);
    lz_vm_set_heap_limit(&vm, vm.heap.live_bytes + bytes);
    LZ_CHECK(lz_method_to_upper(&vm, 1, &arg, &result) == LZ_OK);
    lz_value_release(&vm.heap, &result);
    lz_value_release(&vm.heap, &arg);
    lz_buffer_free(&input); lz_buffer_free(&upper); lz_buffer_free(&lower);
    LZ_CHECK(vm.heap.live_objects == 0 && vm.heap.live_bytes == 0);
    lz_vm_free(&vm);
}

static size_t reference_find(const char *text, size_t length, const char *needle,
                              size_t width, size_t from) {
    if (from > length || width > length - from) return SIZE_MAX;
    for (size_t i = from; i <= length - width; i++) {
        if (width == 0 || memcmp(text + i, needle, width) == 0) return i;
    }
    return SIZE_MAX;
}

static void test_search_corpus(void) {
    LzVM vm;
    lz_vm_init(&vm, (LzWriter){0}, (LzWriter){0});
    uint32_t random = 0x13579BDFu;
    for (size_t round = 0; round < 256; round++) {
        char text[512], needle[7];
        size_t length = text_random(&random) % sizeof text;
        size_t width = round % sizeof needle;
        for (size_t i = 0; i < length; i++) text[i] = "ab:\0\xC3\xA9"[text_random(&random) % 6];
        for (size_t i = 0; i < width; i++) needle[i] = "ab:\0\xC3\xA9"[text_random(&random) % 6];
        LzString *hay = lz_string_new(&vm.heap, text, length);
        LzString *key = lz_string_new(&vm.heap, needle, width);
        LzValue args[] = {lz_value_object(&hay->base), lz_value_object(&key->base)};
        LzValue result = lz_value_nil();
        size_t first = reference_find(text, length, needle, width, 0);
        LZ_CHECK(lz_method_contains(&vm, 2, args, &result) == LZ_OK);
        LZ_CHECK(result.type == LZ_VAL_BOOL && result.as.boolean == (first != SIZE_MAX));
        LZ_CHECK(lz_method_index_of(&vm, 2, args, &result) == LZ_OK);
        int64_t points = -1;
        if (first != SIZE_MAX) {
            points = 0;
            for (size_t i = 0; i < first; i++) points += ((unsigned char)text[i] & 0xC0) != 0x80;
        }
        LZ_CHECK(result.type == LZ_VAL_INT && result.as.integer == points);
        LzStatus status = lz_method_split(&vm, 2, args, &result);
        if (width == 0) {
            LZ_CHECK(status == LZ_ERR_RUNTIME && strstr(vm.error, "separator cannot be empty") != NULL);
        } else {
            LZ_CHECK(status == LZ_OK && lz_value_is_array(&result));
            if (status == LZ_OK && lz_value_is_array(&result)) {
                LzArray *pieces = lz_value_as_array(&result);
                size_t start = 0, count = 0;
                for (;;) {
                    size_t end = reference_find(text, length, needle, width, start);
                    size_t stop = end == SIZE_MAX ? length : end;
                    LZ_CHECK(count < pieces->count);
                    if (count < pieces->count) {
                        LzString *piece = (LzString *)pieces->items[count].as.object;
                        LZ_CHECK(piece->length == stop - start);
                        LZ_CHECK(piece->length != stop - start || memcmp(piece->data, text + start, stop - start) == 0);
                    }
                    count++;
                    if (end == SIZE_MAX) break;
                    start = end + width;
                }
                LZ_CHECK(count == pieces->count);
                lz_value_release(&vm.heap, &result);
            }
        }
        lz_value_release(&vm.heap, &args[0]); lz_value_release(&vm.heap, &args[1]);
    }
    LZ_CHECK(vm.heap.live_objects == 0 && vm.heap.live_bytes == 0);
    lz_vm_free(&vm);
}

static void test_dispatch_cache(void) {
    check_output("fn dupe(x) => x.copy()\n"
                 "for i in 0..4 { println(dupe([1, 2]), dupe({\"a\": 1})) }\n",
                 "[1, 2] {\"a\": 1}\n[1, 2] {\"a\": 1}\n"
                 "[1, 2] {\"a\": 1}\n[1, 2] {\"a\": 1}\n");
    check_output("fn size(x) => x.len()\n"
                 "for i in 0..12 { println(size([1, 2]), size(\"abc\"), size({\"a\": 1})) }\n",
                 "2 3 1\n2 3 1\n2 3 1\n2 3 1\n2 3 1\n2 3 1\n"
                 "2 3 1\n2 3 1\n2 3 1\n2 3 1\n2 3 1\n2 3 1\n");
    Program program;
    run(&program, "let d = {\"a\": 1}\nfor i in 0..20 { d.len() }\n", 0, 0);
    LZ_CHECK(program.compile_status == LZ_OK && program.run_status == LZ_OK);
    LzVM *vm = &program.vm;
    LzString *name = lz_vm_intern(vm, "len", 3);
    const LzValue *method = lz_table_get(&vm->methods, name);
    LZ_CHECK(method != NULL);
    if (method != NULL) {
        const LzNative *native = (const LzNative *)method->as.object;
        LZ_CHECK(native->method_kind == LZ_KIND_DICT && native->method_row != 0);
        LzChunk sites;
        lz_chunk_init(&sites);
        LZ_CHECK(lz_chunk_write(&sites, LZ_OP_METHOD_LOOKUP, 0));
        LZ_CHECK(lz_chunk_register_member_site(&sites, 0));
        LzValue fallback = lz_value_nil();
        LzString *text = lz_vm_intern(vm, "abc", 3);
        LzValue receiver = lz_value_object(&text->base);
        const LzValue *hit = lz_builtin_method_cached(vm, &sites, 0, name, 1, &receiver, &fallback);
        LZ_CHECK(hit != NULL && hit->as.object == method->as.object);
        vm->ext_count = 1;
        LZ_CHECK(lz_builtin_method_cached(vm, &sites, 0, name, 1, &receiver, &fallback) == NULL);
        vm->ext_count = 0;
        for (size_t i = 0; i < 40; i++) {
            char key[32];
            (void)snprintf(key, sizeof key, "cache_probe_%zu", i);
            LzString *interned = lz_vm_intern(vm, key, strlen(key));
            LZ_CHECK(lz_vm_define_method(vm, interned, fallback));
        }
        hit = lz_builtin_method_cached(vm, &sites, 0, name, 1, &receiver, &fallback);
        LZ_CHECK(hit != NULL && hit->as.object == &native->base);
        LzString *upper = lz_vm_intern(vm, "to_upper", 8);
        LzValue replacement = *lz_table_get(&vm->methods, upper);
        LZ_CHECK(lz_vm_define_method(vm, name, replacement));
        hit = lz_builtin_method_cached(vm, &sites, 0, name, 1, &receiver, &fallback);
        LZ_CHECK(hit != NULL && hit->as.object == replacement.as.object);
        lz_chunk_free(&vm->heap, &sites);
    }
    finish(&program);
}

int main(void) {
    test_dispatch_cache();
    test_text_methods();
    test_unicode_text();
    test_number_methods();
    test_universal_and_array_methods();
    test_method_resolution();
    test_no_ufcs();
    test_typeof();
    test_fuel_and_heap();
    test_case_corpus();
    test_search_corpus();
    return LZ_TEST_RESULT();
}
