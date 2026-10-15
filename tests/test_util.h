#ifndef LIZARD_TEST_UTIL_H
#define LIZARD_TEST_UTIL_H

#include <stdio.h>

static int lz_test_failures = 0;

#define LZ_CHECK(condition)                                                                        \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition);          \
            lz_test_failures++;                                                                    \
        }                                                                                          \
    } while (0)

#define LZ_TEST_RESULT() (lz_test_failures == 0 ? 0 : 1)

#endif
