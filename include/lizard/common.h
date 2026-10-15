#ifndef LIZARD_COMMON_H
#define LIZARD_COMMON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Marks "no source position", for example an error with no failing instruction. */
#define LZ_NO_OFFSET UINT32_MAX

typedef struct LzVM LzVM;

typedef enum LzStatus {
    LZ_OK = 0,
    LZ_ERR_ARITY,
    LZ_ERR_MEMORY,
    LZ_ERR_IO,
    LZ_ERR_RUNTIME,
    LZ_ERR_COMPILE,
    /*
     * The script is valid but ran out of a budget the host configured. Hosts
     * can tell these apart from bugs and decide whether to retry or abort.
     */
    LZ_ERR_HEAP_LIMIT,
    LZ_ERR_FUEL,
    LZ_ERR_STACK_OVERFLOW
} LzStatus;

#if defined(__GNUC__) || defined(__clang__)
#define LZ_PRINTF_FORMAT(fmt_index, first_arg) __attribute__((format(printf, fmt_index, first_arg)))
#else
#define LZ_PRINTF_FORMAT(fmt_index, first_arg)
#endif

#endif
