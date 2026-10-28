#include "lizard/skink.h"

#include "lizard/array.h"
#include "lizard/type.h"
#include "lizard/vm.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/*
 * Work units. A trial may spend LZ_SKINK_BUDGET of them: a visited object costs
 * NODE_COST, a reference followed costs EDGE_COST, and looking at the items of an
 * array or the slots of a closure costs one unit per SCAN_BATCH of them. That keeps
 * a trial over a big array of numbers cheap, and a trial over anything bigger than
 * the budget bounded.
 */
/*
 * Test hook: when non-zero, the Nth allocation of working memory for a full pass fails, once. Every
 * failure point can then be tried in turn (tests/test_skink.c) to see that a pass that cannot get
 * its memory gives up cleanly, leaks nothing, and that the next one works.
 */
size_t lz_skink_fault_countdown;

static bool fault(void) {
    return lz_skink_fault_countdown != 0 && --lz_skink_fault_countdown == 0;
}

static void *work_malloc(size_t bytes) {
    return fault() ? NULL : malloc(bytes);
}

static void *work_calloc(size_t count, size_t size) {
    return fault() ? NULL : calloc(count, size);
}

static void *work_realloc(void *old, size_t bytes) {
    return fault() ? NULL : realloc(old, bytes);
}

#define NODE_COST 4
#define EDGE_COST 2
#define SCAN_BATCH 8

/* The ordinary size, on the stack: one trial per frame exit must not allocate. */
typedef struct TrialBuffers {
    LzObject *node[LZ_SKINK_MAX_NODES];
    int64_t rc[LZ_SKINK_MAX_NODES];
    bool live[LZ_SKINK_MAX_NODES];
    uint32_t work[LZ_SKINK_MAX_NODES];
    uint32_t table[LZ_SKINK_MAX_NODES * 2];
} TrialBuffers;

/* Table slots are indexed by uint32_t (index + 1), so a trial can hold this many nodes at most. */
#define FULL_MAX_NODES ((size_t)1 << 30)

typedef struct Trial {
    const LzSkink *skink;
    size_t capacity; /* nodes the arrays have room for: a power of two */
    size_t table_mask;
    size_t budget;
    size_t count;
    size_t units;
    size_t work_top;
    bool growable; /* malloc'd arrays that double when the set fills up: a full pass has no size limit */
    LzObject **node;
    int64_t *rc; /* refcount minus the references that come from inside the set */
    bool *live;
    uint32_t *work;
    uint32_t *table; /* node index + 1; 0 is an empty slot */
} Trial;

typedef bool (*EdgeFn)(Trial *trial, LzObject *target);

typedef enum TrialResult {
    TRIAL_LIVE,         /* something outside still holds the subgraph, or nothing is dead */
    TRIAL_FREED,        /* a dead cycle was found and freed */
    TRIAL_OVER_BUDGET,  /* the subgraph does not fit */
    TRIAL_INCONSISTENT  /* the counts did not add up; nothing was touched */
} TrialResult;

/*
 * An array that is typed with items that can never be an array or a closure
 * (int[], string[], ...) or is empty has no reference that could close a cycle. It is
 * not part of the graph: references to it are simply not followed.
 */
/*
 * An array restricted to a type that cannot hold a closure (lz_type_closure_free) never reaches
 * one, however deep it nests, so it cannot be on a cycle (every cycle runs through a closure): it
 * is a leaf for Skink. Registration uses only this, because the type can only narrow and so the
 * answer never changes; see may_join_cycle.
 */
static bool array_is_typed_leaf(const LzArray *array) {
    return lz_type_closure_free(array->element);
}

/*
 * Whether an array is a node of the graph a trial walks, which is whether it has a reference a
 * cycle could run along *now*. A typed leaf has none. Neither has an array whose
 * may_hold_closures flag is false (a flat array of numbers and strings, annotated or not): no
 * item is a closure or an array that could be given one. Leaving it out is exact, not a guess:
 * a node without an outgoing edge cannot be on a cycle, and the flag turns true in the very
 * store that gives it one, so the next trial sees the edge. Getting it wrong could only make
 * Skink miss a cycle, never free a live object (a reference it does not follow counts as an
 * outside reference), and lz_array_check_flags keeps the flag honest in the tests.
 */
static bool array_may_hold_cycles(const LzArray *array) {
    return array->count != 0 && array->may_hold_closures && !array_is_typed_leaf(array);
}

/* Objects that can take part in a cycle. Strings, natives and functions are leaves. */
static bool is_node(const LzObject *object) {
    switch (object->type) {
    case LZ_OBJ_CLOSURE:
    case LZ_OBJ_UPVALUE:
        return true;
    case LZ_OBJ_ARRAY:
        return array_may_hold_cycles((const LzArray *)object);
    case LZ_OBJ_STRING:
    case LZ_OBJ_NATIVE:
    case LZ_OBJ_FUNCTION:
        break;
    }
    return false;
}

/* A value an edge can lead through: part of the graph a trial walks. */
static bool is_graph_value(const LzValue *value) {
    return value->type == LZ_VAL_OBJECT && value->as.object->type != LZ_OBJ_UPVALUE &&
           is_node(value->as.object);
}

/*
 * A value an upvalue is worth watching for: a closure, or an array that could hold
 * one, even while it is empty (the closure may be pushed into it later).
 */
static bool may_join_cycle(const LzValue *value) {
    if (value->type != LZ_VAL_OBJECT) {
        return false;
    }
    switch (value->as.object->type) {
    case LZ_OBJ_CLOSURE:
        return true;
    case LZ_OBJ_ARRAY:
        return !array_is_typed_leaf((const LzArray *)value->as.object);
    case LZ_OBJ_STRING:
    case LZ_OBJ_NATIVE:
    case LZ_OBJ_FUNCTION:
    case LZ_OBJ_UPVALUE:
        break;
    }
    return false;
}

/* Slots a node has to look through, to price the scan before it starts. */
static size_t slot_count(const LzObject *node) {
    switch (node->type) {
    case LZ_OBJ_CLOSURE:
        return ((const LzClosure *)node)->upvalue_count;
    case LZ_OBJ_ARRAY:
        return ((const LzArray *)node)->count;
    case LZ_OBJ_UPVALUE:
        return 1;
    case LZ_OBJ_STRING:
    case LZ_OBJ_NATIVE:
    case LZ_OBJ_FUNCTION:
        break;
    }
    return 0;
}

/*
 * Calls visit for every reference the node owns to another node. These are exactly
 * the references lz_object_finalize releases, minus the ones to leaves: an array's
 * items, a closure's upvalues, and the value of a closed upvalue. An open upvalue
 * does not own the stack slot it points at.
 */
static bool each_edge(Trial *trial, LzObject *node, EdgeFn visit) {
    switch (node->type) {
    case LZ_OBJ_CLOSURE: {
        const LzClosure *closure = (const LzClosure *)node;
        for (size_t i = 0; i < closure->upvalue_count; i++) {
            if (closure->upvalues[i] != NULL && !visit(trial, &closure->upvalues[i]->base)) {
                return false;
            }
        }
        return true;
    }
    case LZ_OBJ_UPVALUE: {
        const LzUpvalue *upvalue = (const LzUpvalue *)node;
        if (upvalue->location == &upvalue->closed && is_graph_value(&upvalue->closed)) {
            return visit(trial, upvalue->closed.as.object);
        }
        return true;
    }
    case LZ_OBJ_ARRAY: {
        const LzArray *array = (const LzArray *)node;
        for (size_t i = 0; i < array->count; i++) {
            const LzValue *item = &array->items[i];
            if (is_graph_value(item) && !visit(trial, item->as.object)) {
                return false;
            }
        }
        return true;
    }
    case LZ_OBJ_STRING:
    case LZ_OBJ_NATIVE:
    case LZ_OBJ_FUNCTION:
        break;
    }
    return true;
}

/* The references the pending list itself holds: they come from Skink, not from the program. */
static int64_t pending_holds(const LzSkink *skink, const LzObject *object) {
    int64_t holds = 0;
    for (size_t i = 0; i < skink->pending_count; i++) {
        if (&skink->pending[i]->base == object) {
            holds++;
        }
    }
    return holds;
}

static size_t hash_bits(const LzObject *object) {
    uint64_t bits = (uint64_t)((uintptr_t)object >> 4);
    return (size_t)((bits * UINT64_C(0x9E3779B97F4A7C15)) >> 32);
}

static size_t hash_slot(const Trial *trial, const LzObject *object) {
    return hash_bits(object) & trial->table_mask;
}

static bool find_node(const Trial *trial, const LzObject *object, size_t *index) {
    size_t slot = hash_slot(trial, object);
    for (;;) {
        uint32_t entry = trial->table[slot];
        if (entry == 0) {
            return false;
        }
        if (trial->node[entry - 1] == object) {
            *index = (size_t)entry - 1;
            return true;
        }
        slot = (slot + 1) & trial->table_mask;
    }
}

/* A trial whose arrays belong to it, with room for `capacity` nodes to begin with and no limit on the work. */
static bool trial_init_growable(Trial *trial, const LzSkink *skink, size_t capacity) {
    memset(trial, 0, sizeof *trial);
    trial->skink = skink;
    trial->capacity = capacity;
    trial->table_mask = capacity * 2 - 1;
    trial->budget = SIZE_MAX;
    trial->growable = true;
    trial->node = work_malloc(capacity * sizeof *trial->node);
    trial->rc = work_malloc(capacity * sizeof *trial->rc);
    trial->live = work_malloc(capacity * sizeof *trial->live);
    trial->work = work_malloc(capacity * sizeof *trial->work);
    trial->table = work_calloc(capacity * 2, sizeof *trial->table);
    if (trial->node == NULL || trial->rc == NULL || trial->live == NULL || trial->work == NULL ||
        trial->table == NULL) {
        free(trial->node);
        free(trial->rc);
        free(trial->live);
        free(trial->work);
        free(trial->table);
        return false;
    }
    return true;
}

static void trial_free(Trial *trial) {
    if (trial->growable) {
        free(trial->node);
        free(trial->rc);
        free(trial->live);
        free(trial->work);
        free(trial->table);
    }
}

/*
 * Doubles the room of a growable trial and rehashes. False when there is no memory: the trial
 * is then given up, with every array still the right size for the capacity it has.
 */
static bool trial_grow(Trial *trial) {
    size_t capacity = trial->capacity * 2;
    if (!trial->growable || capacity > FULL_MAX_NODES) {
        return false;
    }
    LzObject **node = work_realloc(trial->node, capacity * sizeof *node);
    if (node == NULL) {
        return false;
    }
    trial->node = node;
    int64_t *rc = work_realloc(trial->rc, capacity * sizeof *rc);
    if (rc == NULL) {
        return false;
    }
    trial->rc = rc;
    bool *live = work_realloc(trial->live, capacity * sizeof *live);
    if (live == NULL) {
        return false;
    }
    trial->live = live;
    uint32_t *work = work_realloc(trial->work, capacity * sizeof *work);
    if (work == NULL) {
        return false;
    }
    trial->work = work;
    uint32_t *table = work_calloc(capacity * 2, sizeof *table);
    if (table == NULL) {
        return false;
    }
    size_t mask = capacity * 2 - 1;
    for (size_t i = 0; i < trial->count; i++) {
        size_t slot = hash_bits(trial->node[i]) & mask;
        while (table[slot] != 0) {
            slot = (slot + 1) & mask;
        }
        table[slot] = (uint32_t)(i + 1);
    }
    free(trial->table);
    trial->table = table;
    trial->table_mask = mask;
    trial->capacity = capacity;
    return true;
}

/* The index of the node, adding it with its starting count. False when the set is full. */
static bool intern_node(Trial *trial, LzObject *object, size_t *index) {
    if (find_node(trial, object, index)) {
        return true;
    }
    if (trial->count == trial->capacity && !trial_grow(trial)) {
        return false;
    }
    size_t i = trial->count++;
    trial->node[i] = object;
    trial->rc[i] = (int64_t)object->refcount - pending_holds(trial->skink, object);
    trial->live[i] = false;
    size_t slot = hash_slot(trial, object);
    while (trial->table[slot] != 0) {
        slot = (slot + 1) & trial->table_mask;
    }
    trial->table[slot] = (uint32_t)(i + 1);
    trial->units += NODE_COST;
    *index = i;
    return true;
}

/* A reference from inside the set: it is not an outside reference, so take it off the count. */
static bool visit_discover(Trial *trial, LzObject *target) {
    size_t index;
    if (!intern_node(trial, target, &index)) {
        return false;
    }
    trial->rc[index] -= 1;
    trial->units += EDGE_COST;
    return trial->units <= trial->budget;
}

/* Phase 1: everything reachable from the nodes already in the set, with the internal references subtracted. */
static bool expand_all(Trial *trial) {
    for (size_t i = 0; i < trial->count; i++) {
        trial->units += slot_count(trial->node[i]) / SCAN_BATCH;
        if (trial->units > trial->budget || !each_edge(trial, trial->node[i], visit_discover)) {
            return false;
        }
    }
    return true;
}

static bool discover(Trial *trial, LzObject *root) {
    size_t index;
    return intern_node(trial, root, &index) && expand_all(trial);
}

static bool visit_mark(Trial *trial, LzObject *target) {
    size_t index;
    if (!find_node(trial, target, &index)) {
        return false; /* every reachable node was interned in phase 1 */
    }
    if (!trial->live[index]) {
        trial->live[index] = true;
        trial->work[trial->work_top++] = (uint32_t)index;
    }
    return true;
}

/* Phase 2: a node with a count left is held from outside, and so is all it reaches. */
static bool mark_live(Trial *trial) {
    trial->work_top = 0;
    for (size_t i = 0; i < trial->count; i++) {
        if (trial->rc[i] > 0) {
            trial->live[i] = true;
            trial->work[trial->work_top++] = (uint32_t)i;
        }
    }
    while (trial->work_top > 0) {
        LzObject *node = trial->node[trial->work[--trial->work_top]];
        if (!each_edge(trial, node, visit_mark)) {
            return false;
        }
    }
    return true;
}

/* Drops what the dead node owns, so that releasing the nodes one by one frees them. */
static void clear_edges(LzVM *vm, LzObject *node) {
    switch (node->type) {
    case LZ_OBJ_ARRAY:
        lz_array_clear(&vm->heap, (LzArray *)node);
        break;
    case LZ_OBJ_CLOSURE: {
        LzClosure *closure = (LzClosure *)node;
        for (size_t i = 0; i < closure->upvalue_count; i++) {
            LzUpvalue *upvalue = closure->upvalues[i];
            closure->upvalues[i] = NULL;
            if (upvalue != NULL) {
                lz_object_release(&vm->heap, &upvalue->base);
            }
        }
        break;
    }
    case LZ_OBJ_UPVALUE: {
        LzUpvalue *upvalue = (LzUpvalue *)node;
        if (upvalue->location == &upvalue->closed) {
            LzValue old = upvalue->closed;
            upvalue->closed = lz_value_nil();
            lz_value_release(&vm->heap, &old);
        }
        break;
    }
    case LZ_OBJ_STRING:
    case LZ_OBJ_NATIVE:
    case LZ_OBJ_FUNCTION:
        break;
    }
}

/*
 * Phase 3: frees the dead nodes. Each is retained first, so that dropping the
 * references between them cannot free one while another still points at it.
 */
static size_t collect(LzVM *vm, const Trial *trial) {
    size_t dead = 0;
    for (size_t i = 0; i < trial->count; i++) {
        if (!trial->live[i]) {
            lz_object_retain(trial->node[i]);
            dead++;
        }
    }
    if (dead == 0) {
        return 0;
    }
    for (size_t i = 0; i < trial->count; i++) {
        if (!trial->live[i]) {
            clear_edges(vm, trial->node[i]);
        }
    }
    for (size_t i = 0; i < trial->count; i++) {
        if (!trial->live[i]) {
            lz_object_release(&vm->heap, trial->node[i]);
        }
    }
    return dead;
}

/* Phases 2 and 3 on a set that discover filled: what is held from outside, and what is dead. */
static TrialResult finish_trial(LzVM *vm, Trial *trial, size_t *freed) {
    for (size_t i = 0; i < trial->count; i++) {
        if (trial->rc[i] < 0) {
            return TRIAL_INCONSISTENT; /* more internal references than the object has */
        }
    }
    if (!mark_live(trial)) {
        return TRIAL_INCONSISTENT;
    }
    *freed = collect(vm, trial);
    return *freed > 0 ? TRIAL_FREED : TRIAL_LIVE;
}

/* An ordinary trial: on the stack, with the fixed size and the budget of a tick. */
static TrialResult trial_from(LzVM *vm, LzUpvalue *suspect, size_t *freed) {
    TrialBuffers buffers;
    Trial trial;
    trial.skink = &vm->skink;
    trial.capacity = LZ_SKINK_MAX_NODES;
    trial.table_mask = LZ_SKINK_MAX_NODES * 2 - 1;
    trial.budget = LZ_SKINK_BUDGET;
    trial.count = 0;
    trial.units = 0;
    trial.work_top = 0;
    trial.growable = false;
    trial.node = buffers.node;
    trial.rc = buffers.rc;
    trial.live = buffers.live;
    trial.work = buffers.work;
    trial.table = buffers.table;
    memset(trial.table, 0, sizeof buffers.table);

    if (!discover(&trial, &suspect->base)) {
        return TRIAL_OVER_BUDGET;
    }
    return finish_trial(vm, &trial, freed);
}

static void run_trial(LzVM *vm, LzUpvalue *root) {
    LzSkinkStats *stats = &vm->skink.stats;
    stats->trials++;
    size_t freed = 0;
    switch (trial_from(vm, root, &freed)) {
    case TRIAL_FREED:
        stats->cycles++;
        stats->objects += freed;
        break;
    case TRIAL_OVER_BUDGET:
        stats->over_budget++;
        break;
    case TRIAL_INCONSISTENT:
        stats->inconsistent++;
        break;
    case TRIAL_LIVE:
        break;
    }
}

/* A fresh suspect, still holding the pending list's reference. */
static void examine(LzVM *vm, LzUpvalue *suspect) {
    /* Only the pending list's own reference is left: releasing it frees the upvalue. */
    if (suspect->base.refcount <= 1 || suspect->location != &suspect->closed ||
        !may_join_cycle(&suspect->closed)) {
        return;
    }
    run_trial(vm, suspect);
}

/* A watched upvalue: nothing of Skink's own holds it, and its value may have changed since. */
static void examine_watched(LzVM *vm, LzUpvalue *upvalue) {
    if (upvalue->location != &upvalue->closed || !may_join_cycle(&upvalue->closed)) {
        return;
    }
    vm->skink.stats.polls++;
    run_trial(vm, upvalue);
}

/* ---- watch lists: circular, the head lives in LzSkink, links live in the upvalues ---- */

#define WATCHED_UPVALUE(link) ((LzUpvalue *)((char *)(link) - offsetof(LzUpvalue, watch)))

static void watch_init(LzWatchLink *head) {
    head->prev = head;
    head->next = head;
}

static bool watch_empty(const LzWatchLink *head) {
    return head->next == head;
}

static bool watching(const LzSkink *skink) {
    for (size_t level = 0; level < LZ_SKINK_LEVELS; level++) {
        if (!watch_empty(&skink->watch[level])) {
            return true;
        }
    }
    return false;
}

static void watch_push_back(LzWatchLink *head, LzWatchLink *link) {
    link->prev = head->prev;
    link->next = head;
    head->prev->next = link;
    head->prev = link;
}

/*
 * Level n is due every 4^n ticks. An entry that is polled moves one level down the
 * schedule first, because the trial may free it, and so a live structure is looked
 * at ever more rarely.
 */
static void poll(LzVM *vm) {
    LzSkink *skink = &vm->skink;
    skink->ticks++;
    unsigned polls = LZ_SKINK_POLLS_PER_TICK;
    unsigned refunds = LZ_SKINK_REFUNDS_PER_TICK;
    for (size_t level = 0; level < LZ_SKINK_LEVELS && polls > 0; level++) {
        if ((skink->ticks & ((UINT64_C(1) << (2 * level)) - 1)) != 0) {
            continue;
        }
        const LzWatchLink *first = NULL; /* the last level feeds itself: stop after one lap */
        while (polls > 0 && !watch_empty(&skink->watch[level])) {
            LzWatchLink *front = skink->watch[level].next;
            if (front == first) {
                break;
            }
            if (first == NULL) {
                first = front;
            }
            lz_watch_unlink(front);
            size_t next_level = level + 1 < LZ_SKINK_LEVELS ? level + 1 : level;
            watch_push_back(&skink->watch[next_level], front);
            polls--;
            uint64_t before = skink->stats.objects;
            examine_watched(vm, WATCHED_UPVALUE(front));
            if (skink->stats.objects != before && refunds > 0) {
                refunds--; /* garbage tends to die together: keep going while it is there */
                polls++;
            }
        }
    }
}

/*
 * The fallback when a full pass cannot get its memory: every watched upvalue gets an ordinary trial
 * of its own, and the round is repeated while it still frees something (freeing one cycle can drop
 * the last outside reference of another that was looked at earlier). Entries stay watched, on level 0.
 */
static void bounded_passes(LzVM *vm) {
    LzSkink *skink = &vm->skink;
    for (int pass = 0; pass < 8; pass++) {
        uint64_t before = skink->stats.objects;
        LzWatchLink done; /* entries already examined this pass; an entry that dies unlinks itself */
        watch_init(&done);
        for (size_t level = 0; level < LZ_SKINK_LEVELS; level++) {
            while (!watch_empty(&skink->watch[level])) {
                LzWatchLink *front = skink->watch[level].next;
                lz_watch_unlink(front);
                watch_push_back(&done, front);
                examine_watched(vm, WATCHED_UPVALUE(front));
            }
        }
        while (!watch_empty(&done)) {
            LzWatchLink *front = done.next;
            lz_watch_unlink(front);
            watch_push_back(&skink->watch[0], front);
        }
        if (skink->stats.objects == before) {
            break;
        }
    }
}

/*
 * One trial over everything the watch lists lead to, with no limit on its size: every object is
 * visited once, so the cost is the size of what is reachable, however many upvalues share it. The
 * working memory is malloc'd (outside the heap and its limit) and doubles as needed. False when
 * there was no memory for it, or the counts did not add up: nothing was freed then.
 */
static bool full_pass_once(LzVM *vm, bool during_run) {
    LzSkink *skink = &vm->skink;
    if (!watching(skink)) {
        return true; /* nothing to look at: no pass, no memory, no statistics */
    }
    Trial trial;
    if (!trial_init_growable(&trial, skink, 1024)) {
        return false;
    }
    bool complete = true;
    for (size_t level = 0; level < LZ_SKINK_LEVELS && complete; level++) {
        for (const LzWatchLink *link = skink->watch[level].next;
             link != &skink->watch[level] && complete; link = link->next) {
            LzUpvalue *upvalue = WATCHED_UPVALUE(link);
            size_t index;
            if (upvalue->location == &upvalue->closed && may_join_cycle(&upvalue->closed)) {
                complete = intern_node(&trial, &upvalue->base, &index);
            }
        }
    }
    size_t freed = 0;
    TrialResult result = TRIAL_OVER_BUDGET;
    if (complete && expand_all(&trial)) {
        result = finish_trial(vm, &trial, &freed);
    }
    skink->stats.full_nodes += trial.count;
    trial_free(&trial);
    if (result == TRIAL_INCONSISTENT) {
        skink->stats.inconsistent++;
    }
    if (result != TRIAL_FREED && result != TRIAL_LIVE) {
        return false;
    }
    if (during_run) {
        skink->stats.full_passes++; /* the final pass, once the program is over, is not one of them */
    }
    if (result == TRIAL_FREED) {
        skink->stats.cycles++;
        skink->stats.objects += freed;
    }
    /* What is left was just looked at thoroughly: let it back off to the slowest schedule. */
    for (size_t level = 0; level + 1 < LZ_SKINK_LEVELS; level++) {
        while (!watch_empty(&skink->watch[level])) {
            LzWatchLink *front = skink->watch[level].next;
            lz_watch_unlink(front);
            watch_push_back(&skink->watch[LZ_SKINK_LEVELS - 1], front);
        }
    }
    return true;
}

/* Full passes until one frees nothing (freeing a cycle can release another that held it). False if the first could not run. */
static bool full_collect(LzVM *vm, bool during_run) {
    LzSkink *skink = &vm->skink;
    for (int pass = 0; pass < 8; pass++) {
        uint64_t before = skink->stats.objects;
        if (!full_pass_once(vm, during_run)) {
            return pass > 0;
        }
        if (skink->stats.objects == before) {
            break;
        }
    }
    return true;
}

/* The next growth pass is due when the heap has grown to `factor` times what is left after this one. */
static void schedule_full(LzVM *vm, size_t basis_bytes, size_t factor) {
    size_t next = basis_bytes * factor;
    vm->skink.next_full_bytes = next > LZ_SKINK_FULL_MIN_BYTES ? next : LZ_SKINK_FULL_MIN_BYTES;
}

/* The heap has doubled since the last full pass: whatever is dead and cyclic goes now, however big. */
static void grow_pass(LzVM *vm) {
    LzSkink *skink = &vm->skink;
    size_t before = vm->heap.live_bytes;
    bool ran = full_collect(vm, true);
    size_t after = vm->heap.live_bytes;
    if (after < before) {
        skink->stats.reclaimed += before - after;
    }
    /* Productive (it freed an eighth of the heap or more): stay tight. Idle, or out of memory: back off. */
    bool productive = ran && after < before && (before - after) * 8 >= before;
    schedule_full(vm, ran ? after : before, productive ? LZ_SKINK_FULL_GROWTH : LZ_SKINK_FULL_GROWTH_IDLE);
}

void lz_skink_init(LzSkink *skink) {
    skink->enabled = true;
    skink->final_sweep = true;
    skink->pending_count = 0;
    skink->ticks = 0;
    skink->closed_any = false;
    skink->next_full_bytes = LZ_SKINK_FULL_MIN_BYTES;
    for (size_t i = 0; i < LZ_SKINK_LEVELS; i++) {
        watch_init(&skink->watch[i]);
    }
    memset(&skink->stats, 0, sizeof skink->stats);
}

void lz_skink_note_closed(LzVM *vm, LzUpvalue *upvalue) {
    LzSkink *skink = &vm->skink;
    skink->closed_any = true;
    /* A count of 1 is the VM's open list alone: no closure holds the upvalue, so it dies when
     * the list lets go and no cycle can run through it. */
    if (!skink->enabled || upvalue->base.refcount < 2 || !may_join_cycle(&upvalue->closed)) {
        return;
    }
    if (skink->pending_count == LZ_SKINK_PENDING_MAX) {
        /* No room to hold it for a trial, but the watch list holds nothing: it gets its look later. */
        if (upvalue->watch.next == NULL) {
            watch_push_back(&skink->watch[0], &upvalue->watch);
            skink->stats.watched++;
        }
        skink->stats.dropped++;
        return;
    }
    lz_object_retain(&upvalue->base);
    skink->pending[skink->pending_count++] = upvalue;
    skink->stats.suspects++;
}

void lz_skink_note_store(LzVM *vm, LzUpvalue *upvalue) {
    LzSkink *skink = &vm->skink;
    if (!skink->enabled || upvalue->location != &upvalue->closed || upvalue->watch.next != NULL ||
        !may_join_cycle(&upvalue->closed)) {
        return;
    }
    watch_push_back(&skink->watch[0], &upvalue->watch);
    skink->stats.watched++;
    skink->stats.stored++;
}

void lz_skink_alloc_tick(void *context) {
    LzVM *vm = context;
    LzSkink *skink = &vm->skink;
    /* pending is non-empty only between a close and its run, where nothing allocates; be safe */
    if (!skink->enabled || skink->pending_count > 0) {
        return;
    }
    if (!watching(skink)) {
        return;
    }
    skink->stats.alloc_ticks++;
    if (vm->heap.live_bytes >= skink->next_full_bytes) {
        grow_pass(vm);
    }
    poll(vm);
}

void lz_skink_run(LzVM *vm) {
    LzSkink *skink = &vm->skink;
    /* Every engine closes the same upvalues at the same instructions, but they do not all come
     * through here equally often: only a return that closed something is a tick. */
    if (!skink->closed_any) {
        return;
    }
    skink->closed_any = false;
    /* Poll first: an entry added below was just examined, so its next look is the next tick, when
     * its holder may well have let go. Pending suspects are subtracted by the trials as usual. */
    if (skink->enabled) {
        poll(vm);
    }
    /* A suspect stays in the list while it is examined, so that the references the list
     * holds are not mistaken for references from the program. */
    while (skink->pending_count > 0) {
        LzUpvalue *suspect = skink->pending[skink->pending_count - 1];
        examine(vm, suspect);
        if (suspect->base.refcount > 1 && suspect->watch.next == NULL &&
            may_join_cycle(&suspect->closed)) {
            watch_push_back(&skink->watch[0], &suspect->watch); /* held by something: look again later */
            skink->stats.watched++;
        }
        skink->pending_count--;
        lz_object_release(&vm->heap, &suspect->base);
    }
}

void lz_skink_flush(LzVM *vm) {
    LzSkink *skink = &vm->skink;
    skink->closed_any = false;
    while (skink->pending_count > 0) {
        LzUpvalue *suspect = skink->pending[--skink->pending_count];
        /* The stack is being torn down, so there is no time to look: keep an eye on it instead. */
        if (suspect->base.refcount > 1 && suspect->watch.next == NULL && skink->enabled) {
            watch_push_back(&skink->watch[0], &suspect->watch);
            skink->stats.watched++;
        }
        lz_object_release(&vm->heap, &suspect->base);
    }
}

void lz_skink_sweep(LzVM *vm) {
    if (!vm->skink.enabled || !vm->skink.final_sweep) {
        return;
    }
    if (!full_collect(vm, false)) {
        bounded_passes(vm);
    }
}

size_t lz_skink_reclaim(void *context, size_t needed_bytes) {
    (void)needed_bytes;
    LzVM *vm = context;
    LzSkink *skink = &vm->skink;
    if (!skink->enabled || skink->pending_count > 0 || !watching(skink)) {
        return 0;
    }
    size_t before = vm->heap.live_bytes;
    if (!full_collect(vm, true)) {
        bounded_passes(vm);
    }
    size_t freed = before > vm->heap.live_bytes ? before - vm->heap.live_bytes : 0;
    skink->stats.reclaims++;
    skink->stats.reclaimed += freed;
    schedule_full(vm, vm->heap.live_bytes, LZ_SKINK_FULL_GROWTH);
    return freed;
}
