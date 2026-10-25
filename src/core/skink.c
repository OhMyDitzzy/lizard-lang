#include "lizard/skink.h"

#include "lizard/array.h"
#include "lizard/type.h"
#include "lizard/vm.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/*
 * Work units. A trial may spend LZ_SKINK_BUDGET of them (LZ_SKINK_EMERGENCY_BUDGET in an emergency pass): a visited object costs
 * NODE_COST, a reference followed costs EDGE_COST, and looking at the items of an
 * array or the slots of a closure costs one unit per SCAN_BATCH of them. That keeps
 * a trial over a big array of numbers cheap, and a trial over anything bigger than
 * the budget bounded.
 */
#define NODE_COST 4
#define EDGE_COST 2
#define SCAN_BATCH 8

/* Memory a trial works in: room for `capacity` nodes (a power of two) and twice as many table slots. */
typedef struct Scratch {
    size_t capacity;
    size_t budget;
    LzObject **node;
    int64_t *rc; /* refcount minus the references that come from inside the set */
    bool *live;
    uint32_t *work;
    uint32_t *table; /* node index + 1; 0 is an empty slot */
} Scratch;

/* The ordinary size, on the stack: one trial per frame exit must not allocate. */
typedef struct TrialBuffers {
    LzObject *node[LZ_SKINK_MAX_NODES];
    int64_t rc[LZ_SKINK_MAX_NODES];
    bool live[LZ_SKINK_MAX_NODES];
    uint32_t work[LZ_SKINK_MAX_NODES];
    uint32_t table[LZ_SKINK_MAX_NODES * 2];
} TrialBuffers;

typedef struct Trial {
    const LzSkink *skink;
    size_t capacity;
    size_t table_mask;
    size_t budget;
    size_t count;
    size_t units;
    size_t work_top;
    LzObject **node;
    int64_t *rc;
    bool *live;
    uint32_t *work;
    uint32_t *table;
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
static bool array_is_typed_leaf(const LzArray *array) {
    return array->element != NULL &&
           (lz_type_possible_kinds(array->element) & (LZ_KIND_ARRAY | LZ_KIND_FUNCTION)) == 0;
}

static bool array_may_hold_cycles(const LzArray *array) {
    return array->count != 0 && !array_is_typed_leaf(array);
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

static size_t hash_slot(const Trial *trial, const LzObject *object) {
    uintptr_t bits = (uintptr_t)object >> 4;
    bits ^= bits >> 17;
    return (size_t)(bits * 2654435761u) & trial->table_mask;
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

/* The index of the node, adding it with its starting count. False when the set is full. */
static bool intern_node(Trial *trial, LzObject *object, size_t *index) {
    if (find_node(trial, object, index)) {
        return true;
    }
    if (trial->count == trial->capacity) {
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

/* Phase 1: everything reachable from the root, with the internal references subtracted. */
static bool discover(Trial *trial, LzObject *root) {
    size_t index;
    if (!intern_node(trial, root, &index)) {
        return false;
    }
    for (size_t i = 0; i < trial->count; i++) {
        trial->units += slot_count(trial->node[i]) / SCAN_BATCH;
        if (trial->units > trial->budget || !each_edge(trial, trial->node[i], visit_discover)) {
            return false;
        }
    }
    return true;
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

static TrialResult trial_from(LzVM *vm, LzUpvalue *suspect, size_t *freed, const Scratch *scratch) {
    Trial trial;
    trial.skink = &vm->skink;
    trial.capacity = scratch->capacity;
    trial.table_mask = scratch->capacity * 2 - 1;
    trial.budget = scratch->budget;
    trial.count = 0;
    trial.units = 0;
    trial.work_top = 0;
    trial.node = scratch->node;
    trial.rc = scratch->rc;
    trial.live = scratch->live;
    trial.work = scratch->work;
    trial.table = scratch->table;
    memset(trial.table, 0, (trial.table_mask + 1) * sizeof *trial.table);

    if (!discover(&trial, &suspect->base)) {
        return TRIAL_OVER_BUDGET;
    }
    for (size_t i = 0; i < trial.count; i++) {
        if (trial.rc[i] < 0) {
            return TRIAL_INCONSISTENT; /* more internal references than the object has */
        }
    }
    if (!mark_live(&trial)) {
        return TRIAL_INCONSISTENT;
    }
    *freed = collect(vm, &trial);
    return *freed > 0 ? TRIAL_FREED : TRIAL_LIVE;
}

/* big is the emergency scratch, or NULL for an ordinary trial on the stack. */
static void run_trial(LzVM *vm, LzUpvalue *root, const Scratch *big) {
    LzSkinkStats *stats = &vm->skink.stats;
    TrialBuffers buffers;
    Scratch small = {LZ_SKINK_MAX_NODES, LZ_SKINK_BUDGET, buffers.node, buffers.rc,
                     buffers.live,       buffers.work,    buffers.table};
    stats->trials++;
    size_t freed = 0;
    switch (trial_from(vm, root, &freed, big != NULL ? big : &small)) {
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
    run_trial(vm, suspect, NULL);
}

/* A watched upvalue: nothing of Skink's own holds it, and its value may have changed since. */
static void examine_watched(LzVM *vm, LzUpvalue *upvalue, const Scratch *big) {
    if (upvalue->location != &upvalue->closed || !may_join_cycle(&upvalue->closed)) {
        return;
    }
    vm->skink.stats.polls++;
    run_trial(vm, upvalue, big);
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
            examine_watched(vm, WATCHED_UPVALUE(front), NULL);
        }
    }
}

/*
 * Examines every watched upvalue, and goes round again while a pass still frees something:
 * freeing one cycle can drop the last outside reference of another that was looked at
 * earlier in the pass. Entries stay watched, on level 0, unless they die.
 */
static void watch_passes(LzVM *vm, const Scratch *big) {
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
                examine_watched(vm, WATCHED_UPVALUE(front), big);
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

/* One malloc, outside the heap and its limit. False when there is no memory: the pass then uses the stack. */
static bool scratch_alloc(Scratch *scratch) {
    size_t n = LZ_SKINK_EMERGENCY_NODES;
    size_t bytes = n * sizeof(LzObject *) + n * sizeof(int64_t) + n * 2 * sizeof(uint32_t) +
                   n * sizeof(uint32_t) + n * sizeof(bool);
    char *block = malloc(bytes);
    if (block == NULL) {
        return false;
    }
    scratch->capacity = n;
    scratch->budget = LZ_SKINK_EMERGENCY_BUDGET;
    scratch->node = (LzObject **)(void *)block;
    block += n * sizeof(LzObject *);
    scratch->rc = (int64_t *)(void *)block;
    block += n * sizeof(int64_t);
    scratch->table = (uint32_t *)(void *)block;
    block += n * 2 * sizeof(uint32_t);
    scratch->work = (uint32_t *)(void *)block;
    block += n * sizeof(uint32_t);
    scratch->live = (bool *)(void *)block;
    return true;
}

static void scratch_free(Scratch *scratch) {
    free(scratch->node); /* the block starts with the node array */
}

void lz_skink_init(LzSkink *skink) {
    skink->enabled = true;
    skink->pending_count = 0;
    skink->ticks = 0;
    skink->closed_any = false;
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
    if (!vm->skink.enabled) {
        return;
    }
    Scratch big;
    bool have_big = scratch_alloc(&big);
    watch_passes(vm, have_big ? &big : NULL);
    if (have_big) {
        scratch_free(&big);
    }
}

size_t lz_skink_reclaim(void *context, size_t needed_bytes) {
    (void)needed_bytes;
    LzVM *vm = context;
    LzSkink *skink = &vm->skink;
    if (!skink->enabled || skink->pending_count > 0) {
        return 0;
    }
    size_t before = vm->heap.live_bytes;
    Scratch big;
    bool have_big = scratch_alloc(&big);
    watch_passes(vm, have_big ? &big : NULL);
    if (have_big) {
        scratch_free(&big);
    }
    size_t freed = before > vm->heap.live_bytes ? before - vm->heap.live_bytes : 0;
    skink->stats.reclaims++;
    skink->stats.reclaimed += freed;
    return freed;
}
