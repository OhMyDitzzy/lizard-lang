#include "lizard/memory.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

void lz_heap_init(LzHeap *heap) {
    heap->head = NULL;
    heap->live_objects = 0;
    heap->live_bytes = 0;
    heap->peak_bytes = 0;
    heap->total_allocations = 0;
    heap->limit_bytes = LZ_HEAP_UNLIMITED;
    heap->limit_hit = false;
    heap->trace = false;
}

void lz_heap_destroy(LzHeap *heap) {
    LzObject *object = heap->head;
    while (object != NULL) {
        LzObject *next = object->next;
        if (heap->trace) {
            fprintf(stderr, "[heap] leak  %p type=%d size=%zu refs=%u\n", (void *)object,
                    (int)object->type, object->size, (unsigned)object->refcount);
        }
        free(object);
        object = next;
    }
    heap->head = NULL;
    heap->live_objects = 0;
    heap->live_bytes = 0;
}

void *lz_heap_alloc(LzHeap *heap, LzObjType type, size_t size) {
    assert(size >= sizeof(LzObject));

    if (heap->live_bytes > heap->limit_bytes || size > heap->limit_bytes - heap->live_bytes) {
        heap->limit_hit = true;
        if (heap->trace) {
            fprintf(stderr, "[heap] refuse type=%d size=%zu limit=%zu\n", (int)type, size,
                    heap->limit_bytes);
        }
        return NULL;
    }

    LzObject *object = malloc(size);
    if (object == NULL) {
        return NULL;
    }

    object->prev = NULL;
    object->next = heap->head;
    object->size = size;
    object->refcount = 1;
    object->type = type;
    if (heap->head != NULL) {
        heap->head->prev = object;
    }
    heap->head = object;

    heap->live_objects++;
    heap->live_bytes += size;
    heap->total_allocations++;
    if (heap->live_bytes > heap->peak_bytes) {
        heap->peak_bytes = heap->live_bytes;
    }

    if (heap->trace) {
        fprintf(stderr, "[heap] alloc %p type=%d size=%zu\n", (void *)object, (int)type, size);
    }
    return object;
}

void lz_object_retain(LzObject *object) {
    assert(object->refcount > 0);
    object->refcount++;
}

void lz_object_release(LzHeap *heap, LzObject *object) {
    assert(object->refcount > 0);
    if (--object->refcount != 0) {
        return;
    }

    if (object->prev != NULL) {
        object->prev->next = object->next;
    } else {
        heap->head = object->next;
    }
    if (object->next != NULL) {
        object->next->prev = object->prev;
    }

    heap->live_objects--;
    heap->live_bytes -= object->size;

    if (heap->trace) {
        fprintf(stderr, "[heap] free  %p type=%d size=%zu\n", (void *)object, (int)object->type,
                object->size);
    }
    lz_object_finalize(heap, object);
    free(object);
}
