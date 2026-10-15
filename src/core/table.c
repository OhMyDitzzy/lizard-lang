#include "lizard/table.h"

#include <stdlib.h>
#include <string.h>

static LzTableEntry *find_slot(LzTableEntry *entries, size_t capacity, const LzString *key) {
    size_t index = (size_t)key->hash & (capacity - 1);
    for (;;) {
        LzTableEntry *entry = &entries[index];
        if (entry->key == key || entry->key == NULL) {
            return entry;
        }
        index = (index + 1) & (capacity - 1);
    }
}

static bool grow(LzTable *table) {
    size_t capacity = table->capacity == 0 ? 8 : table->capacity * 2;
    LzTableEntry *entries = calloc(capacity, sizeof *entries);
    if (entries == NULL) {
        return false;
    }

    for (size_t i = 0; i < table->capacity; i++) {
        const LzTableEntry *old = &table->entries[i];
        if (old->key != NULL) {
            *find_slot(entries, capacity, old->key) = *old;
        }
    }

    free(table->entries);
    table->entries = entries;
    table->capacity = capacity;
    return true;
}

void lz_table_init(LzTable *table) {
    table->entries = NULL;
    table->count = 0;
    table->capacity = 0;
}

void lz_table_free(LzHeap *heap, LzTable *table) {
    for (size_t i = 0; i < table->capacity; i++) {
        LzTableEntry *entry = &table->entries[i];
        if (entry->key != NULL) {
            lz_value_release(heap, &entry->value);
            lz_object_release(heap, &entry->key->base);
        }
    }
    free(table->entries);
    lz_table_init(table);
}

bool lz_table_set(LzHeap *heap, LzTable *table, LzString *key, LzValue value) {
    if (table->count + 1 > table->capacity * 3 / 4 && !grow(table)) {
        return false;
    }

    LzTableEntry *entry = find_slot(table->entries, table->capacity, key);
    lz_value_retain(&value);
    if (entry->key == NULL) {
        lz_object_retain(&key->base);
        entry->key = key;
        entry->value = value;
        table->count++;
    } else {
        lz_value_release(heap, &entry->value);
        entry->value = value;
    }
    return true;
}

const LzValue *lz_table_get(const LzTable *table, const LzString *key) {
    if (table->capacity == 0) {
        return NULL;
    }
    const LzTableEntry *entry = find_slot(table->entries, table->capacity, key);
    return entry->key != NULL ? &entry->value : NULL;
}

LzString *lz_table_find_string(const LzTable *table, const char *chars, size_t length,
                               uint32_t hash) {
    if (table->capacity == 0) {
        return NULL;
    }

    size_t index = (size_t)hash & (table->capacity - 1);
    for (;;) {
        LzString *key = table->entries[index].key;
        if (key == NULL) {
            return NULL;
        }
        if (key->hash == hash && key->length == length && memcmp(key->data, chars, length) == 0) {
            return key;
        }
        index = (index + 1) & (table->capacity - 1);
    }
}
