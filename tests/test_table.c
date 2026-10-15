#include <stdio.h>
#include <string.h>

#include "lizard/vm.h"
#include "test_util.h"

static void test_interning(void) {
    LzBuffer out = {0};
    LzVM vm;
    lz_vm_init(&vm, lz_writer_from_buffer(&out), lz_writer_from_buffer(&out));

    LzString *a = lz_vm_intern(&vm, "name", 4);
    LzString *b = lz_vm_intern(&vm, "name", 4);
    LzString *c = lz_vm_intern(&vm, "other", 5);
    LzString *empty = lz_vm_intern(&vm, "", 0);
    LZ_CHECK(a != NULL && c != NULL && empty != NULL);
    LZ_CHECK(a == b);
    LZ_CHECK(a != c);
    LZ_CHECK(a->base.refcount == 1);
    LZ_CHECK(vm.heap.live_objects == 3);
    LZ_CHECK(lz_hash_bytes("name", 4) == a->hash);

    lz_vm_release_roots(&vm);
    LZ_CHECK(vm.heap.live_objects == 0);
    lz_vm_free(&vm);
    lz_buffer_free(&out);
}

static void test_set_get_overwrite_and_growth(void) {
    LzBuffer out = {0};
    LzVM vm;
    lz_vm_init(&vm, lz_writer_from_buffer(&out), lz_writer_from_buffer(&out));

    LzTable table;
    lz_table_init(&table);
    LzString *missing = lz_vm_intern(&vm, "missing", 7);
    LZ_CHECK(lz_table_get(&table, missing) == NULL);

    enum { KEY_COUNT = 200 };
    LzString *keys[KEY_COUNT];
    for (int i = 0; i < KEY_COUNT; i++) {
        char name[16];
        (void)snprintf(name, sizeof name, "k%d", i);
        keys[i] = lz_vm_intern(&vm, name, strlen(name));
        LZ_CHECK(keys[i] != NULL);
        LZ_CHECK(lz_table_set(&vm.heap, &table, keys[i], lz_value_int(i)));
    }
    LZ_CHECK(table.count == KEY_COUNT);
    for (int i = 0; i < KEY_COUNT; i++) {
        const LzValue *value = lz_table_get(&table, keys[i]);
        LZ_CHECK(value != NULL && value->type == LZ_VAL_INT && value->as.integer == i);
    }
    LZ_CHECK(lz_table_get(&table, missing) == NULL);

    LzString *text = lz_string_new(&vm.heap, "old", 3);
    LZ_CHECK(lz_table_set(&vm.heap, &table, keys[0], lz_value_object(&text->base)));
    LZ_CHECK(text->base.refcount == 2);
    LZ_CHECK(lz_table_set(&vm.heap, &table, keys[0], lz_value_int(-1)));
    LZ_CHECK(text->base.refcount == 1);
    LZ_CHECK(table.count == KEY_COUNT);
    lz_object_release(&vm.heap, &text->base);

    lz_table_free(&vm.heap, &table);
    LZ_CHECK(table.entries == NULL && table.count == 0);

    lz_vm_release_roots(&vm);
    LZ_CHECK(vm.heap.live_objects == 0);
    lz_vm_free(&vm);
    lz_buffer_free(&out);
}

int main(void) {
    test_interning();
    test_set_get_overwrite_and_growth();
    return LZ_TEST_RESULT();
}
