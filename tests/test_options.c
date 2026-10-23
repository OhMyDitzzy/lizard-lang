#include <stdint.h>
#include <string.h>

#include "options.h"
#include "test_util.h"

static void test_limits_parse(void) {
    LzOptions options;

    char *plain[] = {"lizard", "--fuel=1000", "--max-heap=64M", "a.lz", NULL};
    LZ_CHECK(lz_options_parse(&options, 4, plain));
    LZ_CHECK(options.has_fuel && options.fuel == 1000);
    LZ_CHECK(options.has_max_heap && options.max_heap_bytes == 64ull * 1024 * 1024);
    LZ_CHECK(options.script_path != NULL && strcmp(options.script_path, "a.lz") == 0);

    char *suffixes[] = {"lizard", "--max-heap=1k", "a.lz", NULL};
    LZ_CHECK(lz_options_parse(&options, 3, suffixes));
    LZ_CHECK(options.max_heap_bytes == 1024);

    char *gigabyte[] = {"lizard", "--max-heap=2G", "a.lz", NULL};
    LZ_CHECK(lz_options_parse(&options, 3, gigabyte));
    LZ_CHECK(options.max_heap_bytes == 2ull * 1024 * 1024 * 1024);

    char *none[] = {"lizard", "a.lz", NULL};
    LZ_CHECK(lz_options_parse(&options, 2, none));
    LZ_CHECK(!options.has_fuel && !options.has_max_heap);
}

static void test_jit_option(void) {
    LzOptions options;
    char *eager[] = {"lizard", "--jit=eager", "a.lz", NULL};
    LZ_CHECK(lz_options_parse(&options, 3, eager));
    LZ_CHECK(options.has_jit && strcmp(options.jit, "eager") == 0);
    char *off[] = {"lizard", "--jit=off", "--engine-stats", "a.lz", NULL};
    LZ_CHECK(lz_options_parse(&options, 4, off));
    LZ_CHECK(options.has_jit && strcmp(options.jit, "off") == 0 && options.engine_stats);
    char *none[] = {"lizard", "a.lz", NULL};
    LZ_CHECK(lz_options_parse(&options, 2, none));
    LZ_CHECK(!options.has_jit);
    char *bad[] = {"lizard", "--jit=maybe", "a.lz", NULL};
    LZ_CHECK(!lz_options_parse(&options, 3, bad));
    LZ_CHECK(strstr(options.error, "--jit") != NULL);
}

static void test_engine_options(void) {
    LzOptions options;

    char *ge[] = {"lizard", "--engine=ge", "a.lz", NULL};
    LZ_CHECK(lz_options_parse(&options, 3, ge));
    LZ_CHECK(options.has_engine && strcmp(options.engine, "ge") == 0);
    LZ_CHECK(!options.engine_stats);

    char *ref[] = {"lizard", "--engine=ref", "--engine-stats", "a.lz", NULL};
    LZ_CHECK(lz_options_parse(&options, 4, ref));
    LZ_CHECK(options.has_engine && strcmp(options.engine, "ref") == 0);
    LZ_CHECK(options.engine_stats);

    char *aliases[] = {"lizard", "--engine=gecko", "a.lz", NULL};
    LZ_CHECK(lz_options_parse(&options, 3, aliases));
    char *reference[] = {"lizard", "--engine=reference", "a.lz", NULL};
    LZ_CHECK(lz_options_parse(&options, 3, reference));

    /* --engine-stats must not be mistaken for --engine=..., and the other way round. */
    char *stats_only[] = {"lizard", "--engine-stats", "a.lz", NULL};
    LZ_CHECK(lz_options_parse(&options, 3, stats_only));
    LZ_CHECK(!options.has_engine && options.engine_stats);

    char *unknown[] = {"lizard", "--engine=jit", "a.lz", NULL};
    LZ_CHECK(!lz_options_parse(&options, 3, unknown));
    LZ_CHECK(strstr(options.error, "--engine") != NULL);

    char *empty[] = {"lizard", "--engine=", "a.lz", NULL};
    LZ_CHECK(!lz_options_parse(&options, 3, empty));

    char *none[] = {"lizard", "a.lz", NULL};
    LZ_CHECK(lz_options_parse(&options, 2, none));
    LZ_CHECK(!options.has_engine && !options.engine_stats);
}

static void test_invalid_values(void) {
    LzOptions options;

    char *letters[] = {"lizard", "--fuel=abc", "a.lz", NULL};
    LZ_CHECK(!lz_options_parse(&options, 3, letters));
    LZ_CHECK(strstr(options.error, "--fuel") != NULL);

    char *empty[] = {"lizard", "--fuel=", "a.lz", NULL};
    LZ_CHECK(!lz_options_parse(&options, 3, empty));

    char *fuel_suffix[] = {"lizard", "--fuel=10k", "a.lz", NULL};
    LZ_CHECK(!lz_options_parse(&options, 3, fuel_suffix));

    char *bad_suffix[] = {"lizard", "--max-heap=12X", "a.lz", NULL};
    LZ_CHECK(!lz_options_parse(&options, 3, bad_suffix));
    LZ_CHECK(strstr(options.error, "--max-heap") != NULL);

    char *double_suffix[] = {"lizard", "--max-heap=1KB", "a.lz", NULL};
    LZ_CHECK(!lz_options_parse(&options, 3, double_suffix));

    char *overflow[] = {"lizard", "--max-heap=99999999999999999999G", "a.lz", NULL};
    LZ_CHECK(!lz_options_parse(&options, 3, overflow));

    char *shift_overflow[] = {"lizard", "--max-heap=17179869184G", "a.lz", NULL};
    LZ_CHECK(!lz_options_parse(&options, 3, shift_overflow));

    char *negative[] = {"lizard", "--fuel=-5", "a.lz", NULL};
    LZ_CHECK(!lz_options_parse(&options, 3, negative));

    char *unknown[] = {"lizard", "--nope", "a.lz", NULL};
    LZ_CHECK(!lz_options_parse(&options, 3, unknown));
}

static void test_script_arguments_are_untouched(void) {
    LzOptions options;
    char *args[] = {"lizard", "--fuel=5", "a.lz", "--fuel=9", "--bogus", NULL};
    LZ_CHECK(lz_options_parse(&options, 5, args));
    LZ_CHECK(options.fuel == 5);
    LZ_CHECK(options.script_argc == 3);
    LZ_CHECK(strcmp(options.script_argv[1], "--fuel=9") == 0);
}

int main(void) {
    test_limits_parse();
    test_engine_options();
    test_jit_option();
    test_invalid_values();
    test_script_arguments_are_untouched();
    return LZ_TEST_RESULT();
}
