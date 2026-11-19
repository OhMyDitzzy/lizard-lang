#include "lizard/memory.h"

#include "lizard/array.h"
#include "lizard/class.h"
#include "lizard/dict.h"
#include "lizard/function.h"
#include "lizard/coroutine.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if LZ_POOL_ENABLED
#define LZ_POOL_CHUNK_BYTES (64u * 1024u)

typedef struct LzPoolChunk {
    struct LzPoolChunk *next;
    size_t used;
    _Alignas(LZ_POOL_STEP) unsigned char data[];
} LzPoolChunk;

static size_t pool_class(size_t size) {
    return (size - 1) / LZ_POOL_STEP;
}

static void *pool_alloc(LzHeap *heap, size_t size) {
    size_t index = pool_class(size);
    size_t stride = (index + 1) * LZ_POOL_STEP;
    LzObject *object = heap->pool_free[index];
    if (object != NULL) {
#ifdef LZ_POISON
        /* The first pointer links free blocks. The rest must still be poisoned. */
        const unsigned char *bytes = (const unsigned char *)object;
        for (size_t i = sizeof object->prev; i < stride; i++) {
            if (bytes[i] != 0xDD) {
                fputs("pooled object modified after free\n", stderr);
                abort();
            }
        }
#endif
        heap->pool_free[index] = object->prev;
        return object;
    }
    LzPoolChunk *chunk = heap->pool_chunks;
    const size_t capacity = LZ_POOL_CHUNK_BYTES - offsetof(LzPoolChunk, data);
    if (chunk == NULL || stride > capacity - chunk->used) {
        chunk = malloc(LZ_POOL_CHUNK_BYTES);
        if (chunk == NULL) {
            return NULL;
        }
        chunk->next = heap->pool_chunks;
        chunk->used = 0;
        heap->pool_chunks = chunk;
    }
    void *block = chunk->data + chunk->used;
    chunk->used += stride;
    return block;
}
#endif

/*
 * Poison mode is enabled with --enable-poison or --poison, which defines LZ_POISON. A raw block that
 * is freed is first filled with 0xDD and parked in a quarantine ring instead of going back to
 * malloc, so that its address is not reused and a stale pointer reads 0xDD. That means refcount
 * 0xDDDDDDDD and a type out of range, which the asserts in lz_object_retain, lz_object_release
 * and lz_link_deref catch. This finds use-after-free without ASan. The ring is flushed by
 * lz_heap_destroy. Pooled blocks are checked before reuse instead, in pool_alloc.
 */
#ifdef LZ_POISON
#define LZ_QUARANTINE_SIZE 4096
static _Thread_local void *quarantine_ring[LZ_QUARANTINE_SIZE];
static _Thread_local size_t quarantine_next;

static void quarantine_flush(void) {
    for (size_t i = 0; i < LZ_QUARANTINE_SIZE; i++) {
        free(quarantine_ring[i]);
        quarantine_ring[i] = NULL;
    }
    quarantine_next = 0;
}

static void quarantine_put(void *block, size_t size) {
    memset(block, 0xDD, size);
    free(quarantine_ring[quarantine_next]); /* the oldest block leaves the quarantine */
    quarantine_ring[quarantine_next] = block;
    quarantine_next = (quarantine_next + 1) % LZ_QUARANTINE_SIZE;
}
#endif

/* Frees a block of `size` bytes. In poison mode it poisons and quarantines it instead. */
void lz_free_sized(void *block, size_t size) {
#ifdef LZ_POISON
    if (block != NULL) {
        quarantine_put(block, size);
    }
#else
    (void)size;
    free(block);
#endif
}

/* Only heap objects use the pool. Owned buffers keep their original allocator. */
static void free_object(LzHeap *heap, LzObject *object) {
#if LZ_POOL_ENABLED
    if (heap->pool_enabled && object->size <= LZ_POOL_MAX) {
        size_t index = pool_class(object->size);
#ifdef LZ_POISON
        memset(object, 0xDD, (index + 1) * LZ_POOL_STEP);
#endif
        object->prev = heap->pool_free[index];
        heap->pool_free[index] = object;
        return;
    }
#else
    (void)heap;
#endif
    lz_free_sized(object, object->size);
}

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
    heap->on_alloc = NULL;
    heap->on_alloc_context = NULL;
    heap->alloc_period = 0;
    heap->alloc_countdown = 0;
    heap->in_hook = false;
    heap->on_link_freed = NULL;
    heap->on_link_freed_context = NULL;
    heap->pool_chunks = NULL;
    memset(heap->pool_free, 0, sizeof heap->pool_free);
#if LZ_POOL_ENABLED
    const char *pool_setting = getenv("LZ_POOL");
    heap->pool_enabled = pool_setting == NULL || strcmp(pool_setting, "off") != 0;
#else
    heap->pool_enabled = false;
#endif
}

/*
 * Invariant: objects still alive here are freed directly, without finalize and without telling
 * the link table. So link cells, which may be among these objects or may already be freed in
 * this loop, can point at freed memory. That is safe only because nothing runs afterwards that
 * reads a cell or a target. lz_vm_free calls lz_link_table_free next, which frees rows only and
 * never dereferences a row's target or a cell. Do not add code after lz_heap_destroy that does.
 */
void lz_heap_destroy(LzHeap *heap) {
    LzObject *object = heap->head;
    while (object != NULL) {
        LzObject *next = object->next;
        if (heap->trace) {
            fprintf(stderr, "[heap] leak  %p type=%d size=%zu refs=%u\n", (void *)object,
                    (int)object->type, object->size, (unsigned)object->refcount);
        }
        if (object->type == LZ_OBJ_ARRAY) {
            lz_free_sized(((LzArray *)object)->items, ((LzArray *)object)->capacity * sizeof(LzValue));
        } else if (object->type == LZ_OBJ_FUNCTION) {
            lz_function_free_storage(heap, (LzFunction *)object, false);
        } else if (object->type == LZ_OBJ_COROUTINE) {
            LzExecutionContext *context = &((LzCoroutine *)object)->context;
            lz_free_sized(context->stack, context->stack_capacity * sizeof *context->stack);
            lz_free_sized(context->frames, context->frame_capacity * sizeof *context->frames);
            lz_free_sized(context->handlers, context->handler_capacity * sizeof *context->handlers);
        }
        free_object(heap, object);
        object = next;
    }
#if LZ_POOL_ENABLED
    while (heap->pool_chunks != NULL) {
        LzPoolChunk *chunk = heap->pool_chunks;
        heap->pool_chunks = chunk->next;
        free(chunk);
    }
#endif
    memset(heap->pool_free, 0, sizeof heap->pool_free);
#ifdef LZ_POISON
    quarantine_flush();
#endif
    heap->head = NULL;
    heap->live_objects = 0;
    heap->live_bytes = 0;
}

static bool over_limit(const LzHeap *heap, size_t bytes) {
    return heap->live_bytes > heap->limit_bytes || bytes > heap->limit_bytes - heap->live_bytes;
}

/* True when bytes fit under the limit, after asking the reclaim hook once if they don't. */
static bool make_room(LzHeap *heap, size_t bytes) {
    if (!over_limit(heap, bytes)) {
        return true;
    }
    if (heap->reclaim == NULL || heap->in_hook) {
        return false;
    }
    heap->in_hook = true;
    (void)heap->reclaim(heap->reclaim_context, bytes);
    heap->in_hook = false;
    return !over_limit(heap, bytes);
}

static void alloc_tick(LzHeap *heap) {
    if (heap->on_alloc == NULL || heap->in_hook || --heap->alloc_countdown != 0) {
        return;
    }
    heap->alloc_countdown = heap->alloc_period;
    heap->in_hook = true;
    heap->on_alloc(heap->on_alloc_context);
    heap->in_hook = false;
}

void *lz_heap_alloc(LzHeap *heap, LzObjType type, size_t size) {
    alloc_tick(heap); /* first: what it frees may be what the limit check below needs */
    assert(size >= sizeof(LzObject));

    if (!make_room(heap, size)) {
        heap->limit_hit = true;
        if (heap->trace) {
            fprintf(stderr, "[heap] refuse type=%d size=%zu limit=%zu\n", (int)type, size,
                    heap->limit_bytes);
        }
        return NULL;
    }

    LzObject *object;
#if LZ_POOL_ENABLED
    object = heap->pool_enabled && size <= LZ_POOL_MAX ? pool_alloc(heap, size) : malloc(size);
#else
    object = malloc(size);
#endif
    if (object == NULL) {
        return NULL;
    }

    object->prev = NULL;
    object->next = heap->head;
    object->size = size;
    object->refcount = 1;
    object->type = type;
    object->trial = 0;
    object->linked = 0;
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
        /* The counter wrapped, so every stamp is cleared and an old mark can't look current. */
        for (LzObject *object = heap->head; object != NULL; object = object->next) {
            if (object->type == LZ_OBJ_ARRAY) {
                lz_array_clear_mark(object);
            } else if (object->type == LZ_OBJ_DICT) {
                lz_dict_clear_mark(object);
            } else if (object->type == LZ_OBJ_INSTANCE) {
                ((LzInstance *)object)->mark = 0;
            }
        }
        heap->epoch = 0;
    }
    return ++heap->epoch;
}

#ifdef LZ_POISON
#define LZ_ASSERT_LIVE(object)                                                                     \
    do {                                                                                           \
        assert((object)->refcount != 0xDDDDDDDDu && (object)->refcount > 0);                       \
        assert((unsigned)(object)->type <= (unsigned)LZ_OBJ_COROUTINE);                            \
    } while (0)
#else
#define LZ_ASSERT_LIVE(object) assert((object)->refcount > 0)
#endif

void lz_object_retain(LzObject *object) {
    LZ_ASSERT_LIVE(object);
    object->refcount++;
}

void lz_object_release(LzHeap *heap, LzObject *object) {
    LZ_ASSERT_LIVE(object);
    if (--object->refcount != 0) {
        return;
    }

    /*
     * The object is dead. Empty the links to it now, before anything else, whether this is the
     * outermost release or one queued while draining. A queued object is finalized and freed
     * later, so a link must not be left pointing at it in between or afterwards. The hook only
     * edits the link table and the cells and releases no object, so it's safe mid-drain.
     */
#ifndef LZ_TEST_REINTRODUCE_LINK_BUG1
    if (object->linked && heap->on_link_freed != NULL) {
        heap->on_link_freed(heap->on_link_freed_context, object);
    }
#endif

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
#ifdef LZ_TEST_REINTRODUCE_LINK_BUG1
    /* The old bug, for tests/poison_selftest.sh: only the outermost release told the table. */
    if (object->linked && heap->on_link_freed != NULL) {
        heap->on_link_freed(heap->on_link_freed_context, object);
    }
#endif
    heap->draining = true;
    lz_object_finalize(heap, object);
    free_object(heap, object);
    while (heap->pending != NULL) {
        LzObject *dead = heap->pending;
        heap->pending = dead->next;
        lz_object_finalize(heap, dead);
        free_object(heap, dead);
    }
    heap->draining = false;
}
