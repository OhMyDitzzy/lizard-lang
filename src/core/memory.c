#include "lizard/memory.h"

#include "lizard/array.h"
#include "lizard/function.h"

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
    heap->pending = NULL;
    heap->draining = false;
    heap->epoch = 0;
    heap->reclaim = NULL;
    heap->reclaim_context = NULL;
    heap->reclaiming = false;
}

void lz_heap_destroy(LzHeap *heap) {
    LzObject *object = heap->head;
    while (object != NULL) {
        LzObject *next = object->next;
        if (heap->trace) {
            fprintf(stderr, "[heap] leak  %p type=%d size=%zu refs=%u\n", (void *)object,
                    (int)object->type, object->size, (unsigned)object->refcount);
        }
        if (object->type == LZ_OBJ_ARRAY) {
            free(((LzArray *)object)->items);
        } else if (object->type == LZ_OBJ_FUNCTION) {
            lz_function_free_storage(heap, (LzFunction *)object, false);
        }
        free(object);
        object = next;
    }
    heap->head = NULL;
    heap->live_objects = 0;
    heap->live_bytes = 0;
}

static bool over_limit(const LzHeap *heap, size_t bytes) {
    return heap->live_bytes > heap->limit_bytes || bytes > heap->limit_bytes - heap->live_bytes;
}

/* True when bytes fit under the limit, after asking the reclaim hook once if they do not. */
static bool make_room(LzHeap *heap, size_t bytes) {
    if (!over_limit(heap, bytes)) {
        return true;
    }
    if (heap->reclaim == NULL || heap->reclaiming) {
        return false;
    }
    heap->reclaiming = true;
    (void)heap->reclaim(heap->reclaim_context, bytes);
    heap->reclaiming = false;
    return !over_limit(heap, bytes);
}

void *lz_heap_alloc(LzHeap *heap, LzObjType type, size_t size) {
    assert(size >= sizeof(LzObject));

    if (!make_room(heap, size)) {
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

bool lz_heap_charge(LzHeap *heap, size_t bytes) {
    if (!make_room(heap, bytes)) {
        heap->limit_hit = true;
        return false;
    }
    heap->live_bytes += bytes;
    if (heap->live_bytes > heap->peak_bytes) {
        heap->peak_bytes = heap->live_bytes;
    }
    return true;
}

void lz_heap_uncharge(LzHeap *heap, size_t bytes) {
    assert(heap->live_bytes >= bytes);
    heap->live_bytes -= bytes;
}

uint32_t lz_heap_next_epoch(LzHeap *heap) {
    if (heap->epoch == UINT32_MAX) {
        /* Wrapped: clear every stamp so an old mark cannot look current. */
        for (LzObject *object = heap->head; object != NULL; object = object->next) {
            if (object->type == LZ_OBJ_ARRAY) {
                lz_array_clear_mark(object);
            }
        }
        heap->epoch = 0;
    }
    return ++heap->epoch;
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

    /*
     * Finalizing a container releases its children. Done recursively, a deeply
     * nested array would overflow the C stack, so a release that happens while
     * another is finalizing is queued and the outermost call drains the queue.
     */
    if (heap->draining) {
        object->next = heap->pending;
        heap->pending = object;
        return;
    }
    heap->draining = true;
    lz_object_finalize(heap, object);
    free(object);
    while (heap->pending != NULL) {
        LzObject *dead = heap->pending;
        heap->pending = dead->next;
        lz_object_finalize(heap, dead);
        free(dead);
    }
    heap->draining = false;
}
