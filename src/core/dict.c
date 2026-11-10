#include "lizard/dict.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lizard/array.h"
#include "lizard/class.h"
#include "lizard/suggest.h"

void lz_dict_clear_mark(LzObject *dict) {
    ((LzDict *)dict)->mark = 0;
}

static uint64_t mix64(uint64_t value) {
    value ^= value >> 33;
    value *= 0xFF51AFD7ED558CCDull;
    value ^= value >> 33;
    value *= 0xC4CEB9FE1A85EC53ull;
    value ^= value >> 33;
    return value;
}

/*
 * Keys are hashed with the dict's own seed, so the bucket a key lands in cannot be
 * predicted from the source and one script cannot make every lookup probe the same
 * run of slots. A collision only costs a comparison: lz_value_equal decides.
 */
static uint64_t hash_key(const LzDict *dict, const LzValue *key) {
    uint64_t part;
    switch (key->type) {
    case LZ_VAL_INT:
        part = mix64((uint64_t)key->as.integer) ^ 0x1111111111111111ull;
        break;
    case LZ_VAL_BOOL:
        part = key->as.boolean ? 0x2222222222222222ull : 0x3333333333333333ull;
        break;
    case LZ_VAL_OBJECT: {
        const LzString *text = (const LzString *)key->as.object;
        part = ((uint64_t)text->hash << 32 | text->hash) ^ 0x4444444444444444ull;
        break;
    }
    default:
        part = 0x5555555555555555ull;
        break;
    }
    return mix64(part ^ dict->seed);
}

static size_t slots_for(size_t entries) {
    size_t slots = 8;
    while (slots < entries * 2) {
        slots *= 2;
    }
    return slots;
}

static void reindex(LzDict *dict) {
    if (dict->slots == NULL) {
        return;
    }
    memset(dict->slots, 0, dict->slot_capacity * sizeof *dict->slots);
    size_t mask = dict->slot_capacity - 1;
    for (size_t i = 0; i < dict->count; i++) {
        size_t at = (size_t)dict->entries[i].hash & mask;
        while (dict->slots[at] != 0) {
            at = (at + 1) & mask;
        }
        dict->slots[at] = (uint32_t)(i + 1);
    }
}

/* Makes room for at least count entries, with the same failure rules as lz_array_reserve. */
static bool reserve(LzHeap *heap, LzDict *dict, size_t count) {
    if (count > LZ_DICT_MAX_LENGTH) {
        return false;
    }
    size_t capacity = dict->capacity < 4 ? 4 : dict->capacity;
    while (capacity < count) {
        capacity *= 2;
    }
    size_t slots = slots_for(capacity);
    if (capacity == dict->capacity && slots == dict->slot_capacity) {
        return true;
    }

    size_t entries_bytes = capacity * sizeof(LzDictEntry);
    size_t slots_bytes = slots * sizeof(uint32_t);
    size_t old_entries = dict->capacity * sizeof(LzDictEntry);
    size_t old_slots = dict->slot_capacity * sizeof(uint32_t);
    size_t extra = 0;
    if (entries_bytes > old_entries) {
        extra += entries_bytes - old_entries;
    }
    if (slots_bytes > old_slots) {
        extra += slots_bytes - old_slots;
    }
    if (!lz_heap_charge(heap, extra)) {
        return false;
    }
    LzDictEntry *entries = realloc(dict->entries, entries_bytes);
    if (entries == NULL) {
        lz_heap_uncharge(heap, extra);
        return false;
    }
    dict->entries = entries;
    uint32_t *grown = realloc(dict->slots, slots_bytes);
    if (grown == NULL) {
        lz_heap_uncharge(heap, extra);
        return false;
    }
    dict->slots = grown;
    dict->capacity = capacity;
    dict->slot_capacity = slots;
    reindex(dict);
    return true;
}

LzDict *lz_dict_new(LzHeap *heap, size_t capacity) {
    if (capacity > LZ_DICT_MAX_LENGTH) {
        return NULL;
    }
    LzDict *dict = lz_heap_alloc(heap, LZ_OBJ_DICT, sizeof *dict);
    if (dict == NULL) {
        return NULL;
    }
    dict->entries = NULL;
    dict->count = 0;
    dict->capacity = 0;
    dict->slots = NULL;
    dict->slot_capacity = 0;
    dict->mark = 0;
    dict->holders = 0;
    dict->key_type = NULL;
    dict->value_type = NULL;
    dict->seed = mix64(lz_heap_next_epoch(heap) * 0x9E3779B97F4A7C15ull);
    if (capacity > 0 && !reserve(heap, dict, capacity)) {
        lz_object_release(heap, &dict->base);
        return NULL;
    }
    return dict;
}

size_t lz_dict_index_of(const LzDict *dict, const LzValue *key) {
    if (dict->count == 0) {
        return SIZE_MAX;
    }
    uint64_t hash = hash_key(dict, key);
    size_t mask = dict->slot_capacity - 1;
    size_t at = (size_t)hash & mask;
    for (;;) {
        uint32_t slot = dict->slots[at];
        if (slot == 0) {
            return SIZE_MAX;
        }
        const LzDictEntry *entry = &dict->entries[slot - 1];
        if (entry->hash == hash && lz_value_equal(&entry->key, key)) {
            return (size_t)(slot - 1);
        }
        at = (at + 1) & mask;
    }
}

LzValue *lz_dict_get(const LzDict *dict, const LzValue *key) {
    size_t index = lz_dict_index_of(dict, key);
    return index == SIZE_MAX ? NULL : &dict->entries[index].value;
}

/* "dewi" for a string key, 20 for an int, true for a bool. */
void lz_dict_write_key(const LzValue *key, const LzWriter *out) {
    if (key->type == LZ_VAL_OBJECT) {
        (void)lz_value_write_debug(key, out);
        return;
    }
    (void)lz_value_write(key, out);
}

LzStatus lz_vm_dict_key_error(LzVM *vm, const LzDict *dict, const LzValue *key,
                              const char *after) {
    LzBuffer text = {0};
    LzWriter out = lz_writer_from_buffer(&text);
    (void)lz_writer_write_cstr(&out, "key ");
    lz_dict_write_key(key, &out);
    (void)lz_writer_write_cstr(&out, " not found");
    if (after != NULL) {
        (void)lz_writer_write_cstr(&out, after);
    }

    LzStatus result = lz_vm_set_error(vm, LZ_ERR_RUNTIME, "%s",
                                      text.data != NULL ? text.data : "key not found");
    lz_buffer_free(&text);
    lz_vm_set_help(vm, "use d.get(key, default) to get a fallback instead of an error");

    /* Only a string key is suggested, and only against the dict's own string keys. */
    if (key->type == LZ_VAL_OBJECT && dict != NULL) {
        LzSuggester suggester;
        const LzString *typed = (const LzString *)key->as.object;
        lz_suggester_init(&suggester, typed->data, typed->length);
        for (size_t i = 0; i < dict->count; i++) {
            const LzValue *existing = &dict->entries[i].key;
            if (existing->type == LZ_VAL_OBJECT) {
                lz_suggester_offer(&suggester, ((const LzString *)existing->as.object)->data);
            }
        }
        vm->error_suggestions = suggester.found;
    }
    return result;
}

LzStatus lz_dict_own_key(LzVM *vm, const LzValue *key) {
    uint16_t kind = lz_kind_of(key);
    if ((kind & (LZ_KIND_STRING | LZ_KIND_INT | LZ_KIND_BOOL)) != 0) {
        return LZ_OK;
    }
    const char *name = lz_value_kind_name(key);
    bool vowel = kind == LZ_KIND_ERROR || (kind == LZ_KIND_INSTANCE && strchr("AEIOU", name[0]) != NULL);
    return lz_vm_set_error(vm, LZ_ERR_RUNTIME, "%s %s cannot be a dict key", vowel ? "an" : "a", name);
}

bool lz_dict_admit_key(const LzDict *dict, const LzValue *key) {
    uint16_t kind = lz_kind_of(key);
    if ((kind & (LZ_KIND_STRING | LZ_KIND_INT | LZ_KIND_BOOL)) == 0) {
        return false;
    }
    return dict->key_type == NULL || lz_type_matches(dict->key_type, key);
}

static LzStatus type_error(LzVM *vm, const LzType *expected, const LzValue *value) {
    LzBuffer expected_text = {0};
    LzBuffer actual = {0};
    LzWriter expected_out = lz_writer_from_buffer(&expected_text);
    LzWriter actual_out = lz_writer_from_buffer(&actual);
    lz_type_describe(expected, &expected_out);
    LzType shown = lz_type_of_value(value);
    if (shown.literal_count > 0) {
        shown = lz_type_widen(&shown);
    }
    lz_type_describe(&shown, &actual_out);
    LzStatus status = lz_vm_set_error(vm, LZ_ERR_TYPE, "type error: dict of %s cannot hold %s",
                                      expected_text.data != NULL ? expected_text.data : "?",
                                      actual.data != NULL ? actual.data : "?");
    lz_buffer_free(&expected_text);
    lz_buffer_free(&actual);
    return status;
}

bool lz_dict_value_fits(const LzDict *dict, const LzValue *value) {
    if (dict->value_type == NULL) {
        return true;
    }
    if (lz_value_is_array(value)) {
        return (dict->value_type->kinds & LZ_KIND_ARRAY) != 0 &&
               lz_array_conform(lz_value_as_array(value), dict->value_type->element);
    }
    if (lz_value_is_dict(value)) {
        return (dict->value_type->kinds & LZ_KIND_DICT) != 0 &&
               lz_dict_conform(lz_value_as_dict(value), dict->value_type->element,
                               dict->value_type->value_element);
    }
    return lz_type_matches(dict->value_type, value);
}

LzStatus lz_dict_check_store(LzVM *vm, LzDict *dict, const LzValue *value) {
    if (lz_value_may_hold_container(value)) {
        uint64_t work = 0;
        int cycle = lz_containers_would_cycle(&vm->heap, value, &dict->base, &work);
        LzStatus charged = lz_vm_charge_fuel(vm, lz_array_cost((size_t)work));
        if (charged != LZ_OK) {
            return charged;
        }
        if (cycle < 0) {
            return lz_vm_out_of_memory(vm);
        }
        if (cycle > 0) {
            return lz_vm_cycle_error(vm, NULL, NULL, 'd');
        }
    }
    if (!lz_dict_value_fits(dict, value)) {
        return type_error(vm, dict->value_type, value);
    }
    return LZ_OK;
}

static void append_entry(LzHeap *heap, LzDict *dict, const LzValue *key, const LzValue *value) {
    LzDictEntry *entry = &dict->entries[dict->count];
    entry->key = *key;
    entry->value = *value;
    entry->hash = hash_key(dict, key);
    lz_value_retain(&entry->key);
    lz_value_retain(&entry->value);
    lz_note_container_store(heap, &entry->value);
    dict->count++;
    size_t mask = dict->slot_capacity - 1;
    size_t at = (size_t)entry->hash & mask;
    while (dict->slots[at] != 0) {
        at = (at + 1) & mask;
    }
    dict->slots[at] = (uint32_t)dict->count;
}

LzStatus lz_dict_set(LzVM *vm, LzDict *dict, const LzValue *key, const LzValue *value) {
    LzStatus status = lz_dict_own_key(vm, key);
    if (status != LZ_OK) {
        return status;
    }
    if (!lz_dict_admit_key(dict, key)) {
        return type_error(vm, dict->key_type, key);
    }
    status = lz_dict_check_store(vm, dict, value);
    if (status != LZ_OK) {
        return status;
    }

    size_t index = lz_dict_index_of(dict, key);
    if (index != SIZE_MAX) {
        /* Overwriting a key keeps its place in the order. */
        LzValue old = dict->entries[index].value;
        lz_value_retain(value);
        dict->entries[index].value = *value;
        lz_note_container_store(&vm->heap, value);
        lz_value_release(&vm->heap, &old);
        return LZ_OK;
    }
    if (dict->count >= LZ_DICT_MAX_LENGTH) {
        return lz_vm_set_error(vm, LZ_ERR_RUNTIME, "dict too large (limit %zu entries)",
                               (size_t)LZ_DICT_MAX_LENGTH);
    }
    if (!reserve(&vm->heap, dict, dict->count + 1)) {
        return lz_vm_out_of_memory(vm);
    }
    append_entry(&vm->heap, dict, key, value);
    return LZ_OK;
}

LzStatus lz_dict_remove(LzVM *vm, LzDict *dict, const LzValue *key, LzValue *out) {
    size_t index = lz_dict_index_of(dict, key);
    if (index == SIZE_MAX) {
        return lz_vm_dict_key_error(vm, dict, key, NULL);
    }
    *out = dict->entries[index].value;
    lz_drop_container_holder(out);
    lz_value_release(&vm->heap, &dict->entries[index].key);
    /* Everything after the hole moves down, and a later add of the key goes to the end. */
    memmove(&dict->entries[index], &dict->entries[index + 1],
            (dict->count - index - 1) * sizeof(LzDictEntry));
    dict->count--;
    reindex(dict);
    return LZ_OK;
}

void lz_dict_clear(LzHeap *heap, LzDict *dict) {
    size_t count = dict->count;
    dict->count = 0;
    for (size_t i = 0; i < count; i++) {
        lz_drop_container_holder(&dict->entries[i].value);
        lz_value_release(heap, &dict->entries[i].key);
        lz_value_release(heap, &dict->entries[i].value);
    }
    if (dict->slots != NULL) {
        memset(dict->slots, 0, dict->slot_capacity * sizeof *dict->slots);
    }
}

bool lz_dict_conform(LzDict *dict, const LzType *keys, const LzType *values) {
    if (dict->key_type == keys && dict->value_type == values) {
        return true;
    }
    /* A dict already narrower than asked keeps its own tag. */
    if (keys != NULL && dict->key_type != NULL &&
        lz_type_assignable(dict->key_type, keys) == LZ_ASSIGN_ALWAYS) {
        keys = dict->key_type;
    }
    if (values != NULL && dict->value_type != NULL &&
        lz_type_assignable(dict->value_type, values) == LZ_ASSIGN_ALWAYS) {
        values = dict->value_type;
    }
    if (keys != NULL) {
        if (dict->key_type != NULL && lz_type_assignable(keys, dict->key_type) != LZ_ASSIGN_ALWAYS) {
            return false; /* restricted to an unrelated type already */
        }
        for (size_t i = 0; i < dict->count; i++) {
            if (!lz_type_matches(keys, &dict->entries[i].key)) {
                return false;
            }
        }
    }
    if (values != NULL) {
        if (dict->value_type != NULL &&
            lz_type_assignable(values, dict->value_type) != LZ_ASSIGN_ALWAYS) {
            return false;
        }
        LzDict probe = *dict;
        probe.value_type = values;
        for (size_t i = 0; i < dict->count; i++) {
            const LzValue *value = &dict->entries[i].value;
            if (lz_value_is_dict(value)) {
                if ((values->kinds & LZ_KIND_DICT) == 0 ||
                    !lz_dict_conform(lz_value_as_dict(value), values->element,
                                     values->value_element)) {
                    return false;
                }
            } else if (!lz_dict_value_fits(&probe, value)) {
                return false;
            }
        }
    }
    dict->key_type = keys;
    dict->value_type = values;
    return true;
}

LzStatus lz_dict_copy(LzVM *vm, const LzDict *dict, LzValue *result) {
    LzStatus charged = lz_vm_charge_fuel(vm, lz_dict_cost(dict->count));
    if (charged != LZ_OK) {
        return charged;
    }
    LzDict *copy = lz_dict_new(&vm->heap, dict->count);
    if (copy == NULL) {
        return lz_vm_out_of_memory(vm);
    }
    for (size_t i = 0; i < dict->count; i++) {
        append_entry(&vm->heap, copy, &dict->entries[i].key, &dict->entries[i].value);
    }
    copy->key_type = dict->key_type;
    copy->value_type = dict->value_type;
    *result = lz_value_object(&copy->base);
    return LZ_OK;
}

LzStatus lz_dict_merge(LzVM *vm, const LzDict *a, const LzDict *b, LzValue *result) {
    LzStatus charged = lz_vm_charge_fuel(vm, lz_dict_cost(a->count + b->count));
    if (charged != LZ_OK) {
        return charged;
    }
    LzDict *merged = lz_dict_new(&vm->heap, a->count + b->count);
    if (merged == NULL) {
        return lz_vm_out_of_memory(vm);
    }
    *result = lz_value_object(&merged->base);
    for (size_t i = 0; i < a->count; i++) {
        LzStatus status = lz_dict_set(vm, merged, &a->entries[i].key, &a->entries[i].value);
        if (status != LZ_OK) {
            return status;
        }
    }
    for (size_t i = 0; i < b->count; i++) {
        /* b wins, but a key it shares keeps a's position. */
        LzStatus status = lz_dict_set(vm, merged, &b->entries[i].key, &b->entries[i].value);
        if (status != LZ_OK) {
            return status;
        }
    }
    return LZ_OK;
}

static LzStatus entries_array(LzVM *vm, const LzDict *dict, bool pairs, LzValue *result) {
    LzStatus charged = lz_vm_charge_fuel(vm, lz_dict_cost(dict->count));
    if (charged != LZ_OK) {
        return charged;
    }
    LzArray *out = lz_array_new(&vm->heap, dict->count);
    if (out == NULL) {
        return lz_vm_out_of_memory(vm);
    }
    *result = lz_value_object(&out->base);
    for (size_t i = 0; i < dict->count; i++) {
        LzValue item = dict->entries[i].key;
        if (pairs) {
            LzArray *pair = lz_array_new(&vm->heap, 2);
            if (pair == NULL) {
                return lz_vm_out_of_memory(vm);
            }
            if (!lz_array_append(&vm->heap, pair, &dict->entries[i].key) ||
                !lz_array_append(&vm->heap, pair, &dict->entries[i].value)) {
                lz_object_release(&vm->heap, &pair->base);
                return lz_vm_out_of_memory(vm);
            }
            item = lz_value_object(&pair->base);
            if (!lz_array_append(&vm->heap, out, &item)) {
                lz_object_release(&vm->heap, &pair->base);
                return lz_vm_out_of_memory(vm);
            }
            lz_object_release(&vm->heap, &pair->base);
            continue;
        }
        if (!lz_array_append(&vm->heap, out, &item)) {
            return lz_vm_out_of_memory(vm);
        }
    }
    return LZ_OK;
}

LzStatus lz_dict_keys(LzVM *vm, const LzDict *dict, LzValue *result) {
    return entries_array(vm, dict, false, result);
}

LzStatus lz_dict_values(LzVM *vm, const LzDict *dict, LzValue *result) {
    LzStatus charged = lz_vm_charge_fuel(vm, lz_dict_cost(dict->count));
    if (charged != LZ_OK) {
        return charged;
    }
    LzArray *out = lz_array_new(&vm->heap, dict->count);
    if (out == NULL) {
        return lz_vm_out_of_memory(vm);
    }
    *result = lz_value_object(&out->base);
    for (size_t i = 0; i < dict->count; i++) {
        if (!lz_array_append(&vm->heap, out, &dict->entries[i].value)) {
            return lz_vm_out_of_memory(vm);
        }
    }
    return LZ_OK;
}

LzStatus lz_dict_items(LzVM *vm, const LzDict *dict, LzValue *result) {
    return entries_array(vm, dict, true, result);
}
