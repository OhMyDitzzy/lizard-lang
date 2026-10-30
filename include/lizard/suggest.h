#ifndef LIZARD_SUGGEST_H
#define LIZARD_SUGGEST_H

#include "lizard/common.h"

/*
 * "Did you mean ...?": finds the names closest to one the program used but that
 * does not exist.
 *
 * The caller feeds the suggester every name that could have been meant (locals,
 * functions of the script, builtins, and later the names an `import` brings in)
 * and reads the best few back. Where the names come from is the caller's business,
 * so a new source of names costs one more loop of lz_suggester_offer calls.
 *
 * Distance is the optimal-string-alignment edit distance (insert, delete,
 * substitute, swap two neighbours) ignoring ASCII case. A name is a candidate when
 * it is within a third of the typed name's length, so `printj` finds both `print`
 * and `println`, while very short names only match when the case differs.
 */

#define LZ_SUGGEST_MAX 3        /* candidates kept */
#define LZ_SUGGEST_NAME_MAX 48  /* longest name handled, terminator included */

typedef struct LzSuggestions {
    size_t count;
    char names[LZ_SUGGEST_MAX][LZ_SUGGEST_NAME_MAX]; /* closest first */
} LzSuggestions;

typedef struct LzSuggester {
    char target[LZ_SUGGEST_NAME_MAX];
    size_t target_length;
    bool active;           /* false when the name is too long to compare */
    unsigned max_distance;
    unsigned distances[LZ_SUGGEST_MAX];
    LzSuggestions found;
} LzSuggester;

/* target is the name that was typed (length bytes, need not be terminated). */
void lz_suggester_init(LzSuggester *suggester, const char *target, size_t length);

/* Offers one candidate name (terminated). Duplicates and the typed name itself are ignored. */
void lz_suggester_offer(LzSuggester *suggester, const char *candidate);

/* The edit distance used above; exposed for tests. */
unsigned lz_edit_distance(const char *a, size_t a_length, const char *b, size_t b_length);

#endif
