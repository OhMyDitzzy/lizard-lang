#include <string.h>

#include "lizard/value.h"
#include "test_util.h"

#if LZ_POOL_ENABLED && defined(LZ_POISON) && (defined(__unix__) || defined(__APPLE__))
#include <signal.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

static void expected_abort(int signal_number) {
    (void)signal_number;
    _Exit(99);
}

static void test_poison_reuse_check(void) {
    pid_t child = fork();
    LZ_CHECK(child >= 0);
    if (child == 0) {
        (void)signal(SIGABRT, expected_abort);
        LzHeap heap;
        lz_heap_init(&heap);
        LzObject *object = lz_heap_alloc(&heap, LZ_OBJ_STRING, 91);
        if (object == NULL) _Exit(1);
        lz_object_release(&heap, object);
        ((unsigned char *)object)[sizeof *object] = 0;
        (void)lz_heap_alloc(&heap, LZ_OBJ_STRING, 91);
        _Exit(0);
    }
    if (child > 0) {
        int status = 0;
        LZ_CHECK(waitpid(child, &status, 0) == child);
        LZ_CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 99);
    }
}
#endif

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

static void check_payload(const LzObject *object, unsigned char pattern) {
    const unsigned char *bytes = (const unsigned char *)object;
    for (size_t i = sizeof *object; i < object->size; i++) {
        LZ_CHECK(bytes[i] == pattern);
    }
}

static LzObject *block(LzHeap *heap, size_t size, unsigned char pattern) {
    /* String finalization has no owned fields, so these blocks need only a header. */
    LzObject *object = lz_heap_alloc(heap, LZ_OBJ_STRING, size);
    LZ_CHECK(object != NULL);
    if (object != NULL) {
        LZ_CHECK((uintptr_t)object % (LZ_POOL_ENABLED ? LZ_POOL_STEP : _Alignof(LzObject)) == 0);
        memset((unsigned char *)object + sizeof *object, pattern, size - sizeof *object);
    }
    return object;
}

static void test_size_classes_and_limits(void) {
    LzHeap heap;
    lz_heap_init(&heap);
    LZ_CHECK(heap.pool_chunks == NULL);
    for (size_t size = sizeof(LzObject); size <= LZ_POOL_MAX + 64; size++) {
        heap.limit_bytes = 2 * size;
        LzObject *a = block(&heap, size, 0x31);
        LzObject *b = block(&heap, size, 0x72);
        LZ_CHECK(heap.live_bytes == 2 * size && heap.live_objects == 2);
        LZ_CHECK(lz_heap_alloc(&heap, LZ_OBJ_STRING, size) == NULL && heap.limit_hit);
        check_payload(a, 0x31);
        check_payload(b, 0x72);
        lz_object_release(&heap, a);
        a = block(&heap, size, 0x93);
        check_payload(b, 0x72);
        check_payload(a, 0x93);
        lz_object_release(&heap, b);
        lz_object_release(&heap, a);
        LZ_CHECK(heap.live_bytes == 0 && heap.head == NULL);
    }
    lz_heap_destroy(&heap);
    LZ_CHECK(heap.pool_chunks == NULL);
    for (size_t i = 0; i < LZ_POOL_CLASSES; i++) LZ_CHECK(heap.pool_free[i] == NULL);
}

static void test_random_lifetimes(void) {
    LzHeap heap;
    lz_heap_init(&heap);
    LzObject *live[256] = {0};
    unsigned char patterns[256] = {0};
    uint32_t random = 0x51C0FFEEu;
    size_t expected = 0;
    for (size_t step = 0; step < 100000; step++) {
        random = random * 1664525u + 1013904223u;
        size_t slot = (random >> 16) % 256;
        if (live[slot] != NULL) {
            check_payload(live[slot], patterns[slot]);
            expected -= live[slot]->size;
            lz_object_release(&heap, live[slot]);
            live[slot] = NULL;
        }
        if ((random & 3u) != 0) {
            size_t size = sizeof(LzObject) + (random >> 8) % 1024;
            patterns[slot] = (unsigned char)(random >> 24);
            live[slot] = block(&heap, size, patterns[slot]);
            expected += size;
        }
        LZ_CHECK(heap.live_bytes == expected);
    }
    for (size_t i = 0; i < 256; i++) {
        if (live[i] != NULL) {
            check_payload(live[i], patterns[i]);
            lz_object_release(&heap, live[i]);
        }
    }
    LZ_CHECK(heap.live_bytes == 0 && heap.live_objects == 0);
    lz_heap_destroy(&heap);
}

static void test_pool_reuse_and_shutdown(void) {
    LzHeap heap;
    lz_heap_init(&heap);
    LzObject *first = block(&heap, 91, 0x31);
    lz_object_release(&heap, first);
    LzObject *again = block(&heap, 92, 0x72);
#if LZ_POOL_ENABLED
    LZ_CHECK(again == first);
#else
    LZ_CHECK(heap.pool_chunks == NULL);
#endif
    lz_object_release(&heap, again);
    LzObject *live[1024];
    for (size_t i = 0; i < 1024; i++) live[i] = block(&heap, 256, (unsigned char)i);
    for (size_t i = 0; i < 1024; i++) check_payload(live[i], (unsigned char)i);
    /* Several chunks still contain live objects. Destruction must reclaim all of them. */
    lz_heap_destroy(&heap);
    LZ_CHECK(heap.pool_chunks == NULL && heap.live_bytes == 0 && heap.head == NULL);
    lz_heap_init(&heap);
    first = block(&heap, 91, 0x93);
    check_payload(first, 0x93);
    lz_object_release(&heap, first);
    lz_heap_destroy(&heap);
}

int main(void) {
    test_refcount_lifecycle();
    test_release_unlinks_from_the_middle();
    test_empty_string_and_destroy_reclaims_leaks();
    test_size_classes_and_limits();
    test_random_lifetimes();
    test_pool_reuse_and_shutdown();
#if LZ_POOL_ENABLED && defined(LZ_POISON) && (defined(__unix__) || defined(__APPLE__))
    test_poison_reuse_check();
#endif
    return LZ_TEST_RESULT();
}
