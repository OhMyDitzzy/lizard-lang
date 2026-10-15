#ifndef LIZARD_TABLE_H
#define LIZARD_TABLE_H

#include "lizard/value.h"

typedef struct LzTableEntry {
    LzString *key;
    LzValue value;
} LzTableEntry;

/*
 * Open-addressing hash table keyed by string identity. Keys must be interned
 * (see lz_vm_intern) so two equal names are the same pointer. The table holds
 * a reference to every key and value it stores. Entries cannot be removed yet.
 */
typedef struct LzTable {
    LzTableEntry *entries;
    size_t count;
    size_t capacity;
} LzTable;

void lz_table_init(LzTable *table);

/* Releases every key and value, then returns the table to its initial empty state. */
void lz_table_free(LzHeap *heap, LzTable *table);

/* Returns false when out of memory. Replacing a value releases the old one. */
bool lz_table_set(LzHeap *heap, LzTable *table, LzString *key, LzValue value);

const LzValue *lz_table_get(const LzTable *table, const LzString *key);

/* Content lookup, used only to implement interning. */
LzString *lz_table_find_string(const LzTable *table, const char *chars, size_t length,
                               uint32_t hash);

#endif
