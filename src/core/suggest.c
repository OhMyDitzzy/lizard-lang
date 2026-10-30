#include "lizard/suggest.h"

#include <string.h>

static char lower(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

unsigned lz_edit_distance(const char *a, size_t a_length, const char *b, size_t b_length) {
    if (a_length >= LZ_SUGGEST_NAME_MAX || b_length >= LZ_SUGGEST_NAME_MAX) {
        return UINT32_MAX; /* callers never compare names this long */
    }

    unsigned d[LZ_SUGGEST_NAME_MAX][LZ_SUGGEST_NAME_MAX];
    for (size_t i = 0; i <= a_length; i++) {
        d[i][0] = (unsigned)i;
    }
    for (size_t j = 0; j <= b_length; j++) {
        d[0][j] = (unsigned)j;
    }

    for (size_t i = 1; i <= a_length; i++) {
        for (size_t j = 1; j <= b_length; j++) {
            unsigned cost = lower(a[i - 1]) == lower(b[j - 1]) ? 0u : 1u;
            unsigned best = d[i - 1][j] + 1;                    /* delete */
            if (d[i][j - 1] + 1 < best) {
                best = d[i][j - 1] + 1;                         /* insert */
            }
            if (d[i - 1][j - 1] + cost < best) {
                best = d[i - 1][j - 1] + cost;                  /* substitute */
            }
            if (i > 1 && j > 1 && lower(a[i - 1]) == lower(b[j - 2]) &&
                lower(a[i - 2]) == lower(b[j - 1]) && d[i - 2][j - 2] + 1 < best) {
                best = d[i - 2][j - 2] + 1;                     /* swap neighbours */
            }
            d[i][j] = best;
        }
    }
    return d[a_length][b_length];
}

void lz_suggester_init(LzSuggester *suggester, const char *target, size_t length) {
    memset(suggester, 0, sizeof *suggester);
    if (length == 0 || length >= LZ_SUGGEST_NAME_MAX) {
        return;
    }
    memcpy(suggester->target, target, length);
    suggester->target[length] = '\0';
    suggester->target_length = length;
    suggester->active = true;
    /* One typo per three letters; one or two letters only forgive a wrong case. */
    suggester->max_distance = length <= 2 ? 0u : (unsigned)(length / 3 > 0 ? length / 3 : 1);
}

void lz_suggester_offer(LzSuggester *suggester, const char *candidate) {
    if (!suggester->active || candidate == NULL) {
        return;
    }
    size_t length = strlen(candidate);
    if (length == 0 || length >= LZ_SUGGEST_NAME_MAX || strcmp(candidate, suggester->target) == 0) {
        return;
    }

    /* A name whose length differs by more than the budget cannot be close enough. */
    size_t gap = length > suggester->target_length ? length - suggester->target_length
                                                   : suggester->target_length - length;
    if (gap > suggester->max_distance) {
        return;
    }
    unsigned distance = lz_edit_distance(suggester->target, suggester->target_length, candidate, length);
    if (distance > suggester->max_distance) {
        return;
    }

    LzSuggestions *found = &suggester->found;
    for (size_t i = 0; i < found->count; i++) {
        if (strcmp(found->names[i], candidate) == 0) {
            return; /* the same name arrives from a local and from the builtins */
        }
    }

    /* Insertion sort into the top LZ_SUGGEST_MAX: closest first, then alphabetical. */
    size_t at = found->count;
    while (at > 0 && (suggester->distances[at - 1] > distance ||
                      (suggester->distances[at - 1] == distance &&
                       strcmp(found->names[at - 1], candidate) > 0))) {
        at--;
    }
    if (at >= LZ_SUGGEST_MAX) {
        return;
    }
    size_t last = found->count < LZ_SUGGEST_MAX ? found->count : LZ_SUGGEST_MAX - 1;
    for (size_t i = last; i > at; i--) {
        memcpy(found->names[i], found->names[i - 1], LZ_SUGGEST_NAME_MAX);
        suggester->distances[i] = suggester->distances[i - 1];
    }
    memcpy(found->names[at], candidate, length + 1);
    suggester->distances[at] = distance;
    if (found->count < LZ_SUGGEST_MAX) {
        found->count++;
    }

    /* Only names about as close as the best one stay: `Println` is `println`, not `print`. */
    while (found->count > 1 && suggester->distances[found->count - 1] > suggester->distances[0] + 1) {
        found->count--;
    }
}
