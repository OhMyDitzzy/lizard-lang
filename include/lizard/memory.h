#ifndef LIZARD_MEMORY_H
#define LIZARD_MEMORY_H

#include "lizard/common.h"

typedef enum LzObjType {
    LZ_OBJ_STRING,
    LZ_OBJ_NATIVE,
    LZ_OBJ_ARRAY,
    LZ_OBJ_FUNCTION, /* the compiled code of a user function; never a value of its own */
    LZ_OBJ_CLOSURE,  /* a callable user function: a function plus the variables it captured */
    LZ_OBJ_UPVALUE   /* a variable shared between a function and the closures that captured it */
} LzObjType;

/*
 * Header shared by every heap object. The heap keeps all live objects in a
 * doubly linked list so it can account for them and report leaks at shutdown.
 */
typedef struct LzObject {
    struct LzObject *prev;
    struct LzObject *next;
    size_t size;
    uint32_t refcount;
    LzObjType type;
} LzObject;

#define LZ_HEAP_UNLIMITED SIZE_MAX

/*
 * limit_bytes caps live_bytes, which counts heap objects (strings, natives and
 * future containers). Tables and chunks owned by the VM or the program are not
 * counted. limit_hit is set when an allocation was refused because of the
 * limit rather than because malloc failed.
 */
typedef struct LzHeap {
    LzObject *head;
    size_t live_objects;
    size_t live_bytes;
    size_t peak_bytes;
    size_t total_allocations;
    size_t limit_bytes;
    bool limit_hit;
    bool trace;
    LzObject *pending; /* dead objects whose release is deferred, see lz_object_release */
    bool draining;
    uint32_t epoch; /* visit stamp for graph walks over arrays, see lz_heap_next_epoch */
    /*
     * Optional: asked for room before an allocation is refused because of the limit. It
     * frees what it can (the VM's cycle collector) and the allocation is tried once more.
     * It is never re-entered, and an allocation can happen anywhere, so it may free only
     * what nothing references.
     */
    size_t (*reclaim)(void *context, size_t needed_bytes);
    void *reclaim_context;
    /*
     * Optional: called every alloc_period object allocations, before the allocation is made,
     * so that the VM's cycle collector gets a turn in a program that never closes an upvalue.
     * Allocations are counted by the program, not by the clock, so every engine ticks at the
     * same allocations. Same rules as reclaim: it may run anywhere, and frees only what
     * nothing references.
     */
    void (*on_alloc)(void *context);
    void *on_alloc_context;
    uint32_t alloc_period;
    uint32_t alloc_countdown;
    bool in_hook; /* a hook is running: neither is re-entered */
} LzHeap;

void lz_heap_init(LzHeap *heap);

/* Frees every object still alive. Callers read live_objects first to detect leaks. */
void lz_heap_destroy(LzHeap *heap);

/* Returns an object with refcount 1, or NULL when out of memory or over the limit. size includes the header. */
void *lz_heap_alloc(LzHeap *heap, LzObjType type, size_t size);

/*
 * Releases whatever references a dying object holds. Implemented in value.c,
 * which knows every object layout; the heap itself stays layout-agnostic.
 */
void lz_object_finalize(LzHeap *heap, LzObject *object);

/*
 * Accounts for memory a heap object owns outside its own block (an array's
 * item buffer). Fails, and sets limit_hit, when it would pass the limit.
 * Every successful charge is paired with an uncharge of the same size.
 */
bool lz_heap_charge(LzHeap *heap, size_t bytes);
void lz_heap_uncharge(LzHeap *heap, size_t bytes);

/* A fresh stamp for a walk over arrays; arrays marked with it have been visited. */
uint32_t lz_heap_next_epoch(LzHeap *heap);

void lz_object_retain(LzObject *object);
void lz_object_release(LzHeap *heap, LzObject *object);

#endif
