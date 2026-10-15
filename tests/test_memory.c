#include <string.h>

#include "lizard/value.h"
#include "test_util.h"

static void test_refcount_lifecycle(void) {
    LzHeap heap;
    lz_heap_init(&heap);

    LzString *text = lz_string_new(&heap, "abc", 3);
    LZ_CHECK(text != NULL);
    LZ_CHECK(text->length == 3);
    LZ_CHECK(strcmp(text->data, "abc") == 0);
    LZ_CHECK(text->base.refcount == 1);
    LZ_CHECK(heap.live_objects == 1);
    LZ_CHECK(heap.live_bytes == sizeof(LzString) + 4);

    size_t bytes = heap.live_bytes;
    lz_object_retain(&text->base);
    LZ_CHECK(text->base.refcount == 2);

    lz_object_release(&heap, &text->base);
    LZ_CHECK(heap.live_objects == 1);

    lz_object_release(&heap, &text->base);
    LZ_CHECK(heap.live_objects == 0);
    LZ_CHECK(heap.live_bytes == 0);
    LZ_CHECK(heap.peak_bytes == bytes);

    lz_heap_destroy(&heap);
}

static void test_release_unlinks_from_the_middle(void) {
    LzHeap heap;
    lz_heap_init(&heap);

    LzString *first = lz_string_new(&heap, "a", 1);
    LzString *second = lz_string_new(&heap, "b", 1);
    LzString *third = lz_string_new(&heap, "c", 1);
    LZ_CHECK(heap.live_objects == 3);

    lz_object_release(&heap, &second->base);
    LZ_CHECK(heap.live_objects == 2);
    lz_object_release(&heap, &third->base);
    lz_object_release(&heap, &first->base);
    LZ_CHECK(heap.live_objects == 0);
    LZ_CHECK(heap.head == NULL);

    lz_heap_destroy(&heap);
}

static void test_empty_string_and_destroy_reclaims_leaks(void) {
    LzHeap heap;
    lz_heap_init(&heap);

    LzString *empty = lz_string_new(&heap, NULL, 0);
    LZ_CHECK(empty != NULL);
    LZ_CHECK(empty->length == 0);
    LZ_CHECK(empty->data[0] == '\0');
    LZ_CHECK(lz_string_new(&heap, "leaked", 6) != NULL);
    LZ_CHECK(heap.live_objects == 2);

    lz_heap_destroy(&heap);
    LZ_CHECK(heap.live_objects == 0);
    LZ_CHECK(heap.head == NULL);
}

int main(void) {
    test_refcount_lifecycle();
    test_release_unlinks_from_the_middle();
    test_empty_string_and_destroy_reclaims_leaks();
    return LZ_TEST_RESULT();
}
