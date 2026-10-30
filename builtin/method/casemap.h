#ifndef LIZARD_BUILTIN_CASEMAP_H
#define LIZARD_BUILTIN_CASEMAP_H

#include <stddef.h>
#include <stdint.h>

/* Unicode simple case mapping of one code point; the code point itself when it has none. */
uint32_t lz_case_upper(uint32_t cp);
uint32_t lz_case_lower(uint32_t cp);

#endif
