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
    test_invalid_values();
    test_script_arguments_are_untouched();
    return LZ_TEST_RESULT();
}
