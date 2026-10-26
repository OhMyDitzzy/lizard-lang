#ifndef LIZARD_SKINK_H
#define LIZARD_SKINK_H

#include "lizard/function.h"

struct LzVM;

/*
 * Skink, the cycle collector of Lizard.
 *
 * Reference counting frees everything except cycles. In Lizard a cycle can only
 * be built through a closure and one of its upvalues (an array that holds itself
 * is refused when it is stored), and the usual moment it closes is when a frame
 * returns and the captured variable moves from the stack into the upvalue. Skink
 * looks for garbage exactly there, like a lizard shedding its tail where it
 * breaks off, and never scans the whole heap:
 *
 *   1. lz_vm_close_upvalues reports every upvalue it closes whose value may take
 *      part in a cycle (a closure, or an array that may hold one). The report
 *      holds a reference, because the frame's own slots are still alive then.
 *   2. Once the frame is gone (lz_skink_run), each suspect gets a local trial
 *      deletion: only the objects reachable from it are visited, the references
 *      between them are subtracted from their counts, and whatever is left
 *      without an outside reference is a dead cycle and is freed on the spot.
 *   3. A trial has a fixed budget. A subgraph that does not fit is left alone
 *      (nothing is ever freed on a guess), so the cost of one exit is bounded
 *      by a constant, whatever the size of the heap.
 *
 *   4. A suspect that survives its trial (something still holds it) goes on a watch
 *      list and is examined again later, with a backoff: every tick, then every 4th,
 *      16th and 64th. That is how a cycle is found whose last outside reference is
 *      dropped after the frame exit, or that was completed after its upvalue closed
 *      (an array the closure captured gets the closure pushed into it). A tick is a
 *      call of lz_skink_run that follows the closing of at least one upvalue (the
 *      reference interpreter returns through lz_vm_return every time, the Gecko
 *      Engine only when it has something to close, and the ticks must not differ), and one tick examines at most LZ_SKINK_POLLS_PER_TICK
 *      watched upvalues, so the cost of a tick is bounded too. The lists hold no
 *      references: an upvalue unlinks itself when it is freed.
 *   5. lz_skink_sweep examines the whole watch list once the roots are gone, so a
 *      cycle that was only waiting for the program to end is freed too.
 *
 *   6. Before an allocation is refused because of --max-heap, the heap asks
 *      lz_skink_reclaim for room. That examines the whole watch list, with a much
 *      bigger budget (LZ_SKINK_EMERGENCY_*) than a tick may spend, because a
 *      slow collection beats a heap-limit error. Garbage that was waiting for its
 *      next poll, or was too big for a trial, is freed there and the allocation
 *      goes on.
 *
 *   7. An upvalue that held a number, a string or nil when its frame ended was never
 *      reported. SET_UPVALUE tells Skink when such a closed upvalue is assigned a
 *      closure or an array (lz_skink_note_store), and it goes on the watch list then.
 *
 *   8. Every LZ_SKINK_ALLOC_PERIOD object allocations the heap calls
 *      lz_skink_alloc_tick, which is one more tick of the same clock. A program
 *      that builds cycles, drops them and goes on allocating without ever closing an
 *      upvalue again still gets them polled. A poll that frees something is refunded
 *      (up to LZ_SKINK_REFUNDS_PER_TICK), so a burst of dead cycles is cleared in a
 *      few ticks instead of two per tick.
 *
 * Not covered: a cycle that dies in a program that neither closes an upvalue nor allocates
 * again (nothing ticks until the heap limit or the end of the program asks).
 */

#define LZ_SKINK_PENDING_MAX 64 /* suspects one frame exit can report; more are skipped */
#define LZ_SKINK_MAX_NODES 128  /* objects one trial can hold */
#define LZ_SKINK_BUDGET 1024    /* work units one trial may spend, see skink.c */
#define LZ_SKINK_LEVELS 4       /* watch lists; level n is polled every 4^n ticks */
#define LZ_SKINK_POLLS_PER_TICK 2
#define LZ_SKINK_REFUNDS_PER_TICK 30 /* a poll that frees something does not count against the 2 */
#ifndef LZ_SKINK_ALLOC_PERIOD
#define LZ_SKINK_ALLOC_PERIOD 128    /* allocations between two ticks that need no upvalue to close */
#endif
#define LZ_SKINK_EMERGENCY_NODES 4096     /* a power of two */
#define LZ_SKINK_EMERGENCY_BUDGET 65536   /* scratch memory for it is malloc'd, outside the heap limit */

typedef struct LzSkinkStats {
    uint64_t suspects;     /* upvalues reported at a frame exit */
    uint64_t watched;      /* upvalues put on a watch list: survivors of a trial, overflow, assignments */
    uint64_t stored;       /* of those, closed upvalues that were assigned a container after they closed */
    uint64_t polls;        /* trials started from a watch list (they are counted in trials too) */
    uint64_t alloc_ticks;  /* ticks that came from the allocation count, not from a frame exit */
    uint64_t dropped;      /* suspects past the pending list: watched without a trial */
    uint64_t trials;       /* trial deletions started */
    uint64_t over_budget;  /* trials abandoned because the subgraph was too big */
    uint64_t inconsistent; /* trials abandoned because the counts did not add up (a bug) */
    uint64_t cycles;       /* trials that found and freed a dead cycle */
    uint64_t objects;      /* objects freed that way */
    uint64_t reclaims;     /* emergency passes, before --max-heap refused an allocation */
    uint64_t reclaimed;    /* heap bytes those passes freed */
} LzSkinkStats;

typedef struct LzSkink {
    bool enabled;
    size_t pending_count;
    LzUpvalue *pending[LZ_SKINK_PENDING_MAX]; /* every entry owns one reference */
    uint64_t ticks;
    bool closed_any; /* an upvalue was closed since the last lz_skink_run */
    LzWatchLink watch[LZ_SKINK_LEVELS]; /* circular lists; the element in the array is the head */
    LzSkinkStats stats;
} LzSkink;

void lz_skink_init(LzSkink *skink); /* enabled; the struct must not move afterwards (the lists point into it) */

/* Called by lz_vm_close_upvalues for an upvalue it has just closed, before the list's reference is dropped. */
void lz_skink_note_closed(struct LzVM *vm, LzUpvalue *upvalue);

/*
 * Called by SET_UPVALUE after it stored an object. A closed upvalue that held no
 * container when its frame ended was never reported; this is where it learns to be
 * watched. (An open upvalue is the stack slot's business: if it closes holding the
 * object, lz_vm_close_upvalues reports it.)
 */
void lz_skink_note_store(struct LzVM *vm, LzUpvalue *upvalue);

/* The heap's allocation hook (context is the LzVM): one tick of the watch lists, when there is anything on them. */
void lz_skink_alloc_tick(void *vm);

/* Examines and releases every reported suspect. Call once the closing frame's slots are gone. */
void lz_skink_run(struct LzVM *vm);

/* Releases the reports without examining them (the stack is being torn down); live ones go on the watch list. */
void lz_skink_flush(struct LzVM *vm);

/*
 * The heap's reclaim hook (context is the LzVM): an emergency pass over the watch list.
 * Returns the heap bytes it freed.
 */
size_t lz_skink_reclaim(void *vm, size_t needed_bytes);

/* Examines every watched upvalue until a pass frees nothing. Call after the roots are released. */
void lz_skink_sweep(struct LzVM *vm);

#endif
