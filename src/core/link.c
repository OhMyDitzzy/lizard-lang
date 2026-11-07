#include "lizard/link.h"

#include "lizard/memory.h"
#include "lizard/vm.h"

#include <stdio.h>
#include <stdlib.h>

/* The heap hook: the VM is the context, the object is the link target that is about to die. */
void lz_link_release_hook(void *context, LzObject *object) {
    lz_link_target_freed((LzVM *)context, object);
}

/*
 * The link table: a hash of target -> the cells that point at it. Only fields declared `link`
 * and calls of link() add rows, so the table is small next to the heap; each operation touches
 * one bucket and one row, whatever the heap holds. Rows are allocated here (not on the heap).
 */
static size_t link_hash(const LzObject *target, size_t buckets) {
    uintptr_t value = (uintptr_t)target;
    value ^= value >> 17;
    value *= (uintptr_t)0x9e3779b97f4a7c15u;
    value ^= value >> 29;
    return (size_t)value & (buckets - 1);
}

LzLink *lz_link_new(LzHeap *heap, LzObject *target) {
    LzLink *link = lz_heap_alloc(heap, LZ_OBJ_LINK, sizeof *link);
    if (link == NULL) {
        return NULL;
    }
    link->base.linked = 0;
    link->target = target;
    link->next = NULL;
    return link;
}

bool lz_link_target_ok(const LzValue *value) {
    if (value->type != LZ_VAL_OBJECT) {
        return value->type == LZ_VAL_NIL;
    }
    switch (value->as.object->type) {
    case LZ_OBJ_INSTANCE:
    case LZ_OBJ_ARRAY:
    case LZ_OBJ_DICT:
    case LZ_OBJ_CLOSURE:
    case LZ_OBJ_FUNCTION:
        return true;
    default:
        return false;
    }
}

static LzLinkRow *row_find(const LzVM *vm, const LzObject *target) {
    if (vm->link_bucket_count == 0) {
        return NULL;
    }
    LzLinkRow *row = vm->link_buckets[link_hash(target, vm->link_bucket_count)];
    while (row != NULL && row->target != target) {
        row = row->next;
    }
    return row;
}

/* Grows the bucket array; a failure leaves the table as it was (rows stay reachable). */
static void table_grow(LzVM *vm) {
    size_t capacity = vm->link_bucket_count == 0 ? 16 : vm->link_bucket_count * 2;
    LzLinkRow **buckets = calloc(capacity, sizeof *buckets);
    if (buckets == NULL) {
        return;
    }
    for (size_t i = 0; i < vm->link_bucket_count; i++) {
        LzLinkRow *row = vm->link_buckets[i];
        while (row != NULL) {
            LzLinkRow *next = row->next;
            size_t bucket = link_hash(row->target, capacity);
            row->next = buckets[bucket];
            buckets[bucket] = row;
            row = next;
        }
    }
    free(vm->link_buckets);
    vm->link_buckets = buckets;
    vm->link_bucket_count = capacity;
}

/* Test hook: when N > 0, the Nth next attempt to make a row fails as if malloc had. */
int lz_link_fail_next_row = 0;

static bool row_alloc_fails(void) {
    if (lz_link_fail_next_row > 0 && --lz_link_fail_next_row == 0) {
        return true;
    }
    return false;
}

bool lz_link_track(LzVM *vm, LzLink *link) {
    LzObject *target = link->target;
    if (target == NULL) {
        return true;
    }
    if (vm->link_count >= vm->link_bucket_count * 2) {
        table_grow(vm); /* may fail: the chains just get longer */
    }
    LzLinkRow *row = row_find(vm, target);
    if (row == NULL) {
        if (vm->link_bucket_count == 0) {
            table_grow(vm);
        }
        row = row_alloc_fails() ? NULL : malloc(sizeof *row);
        if (row == NULL || vm->link_bucket_count == 0) {
            free(row);
            /* Not registered, so it could never be emptied: it must not point at the target. */
            link->target = NULL;
            link->next = NULL;
            return false;
        }
        row->target = target;
        row->head = NULL;
        row->count = 0;
        size_t bucket = link_hash(target, vm->link_bucket_count);
        row->next = vm->link_buckets[bucket];
        vm->link_buckets[bucket] = row;
        vm->link_targets++;
        target->linked = 1;
    }
    link->next = row->head;
    row->head = link;
    row->count++;
    vm->link_count++;
    return true;
}

/*
 * Removes a dying cell from the table. Counts change only when the cell is really found in its
 * target's row. A cell whose target is NULL (emptied, or never registered) is not in the table.
 * Invariant: a row exists only for a live target, so while the row is found, `target` is live
 * and may be touched; the target is never read when no row is found.
 */
void lz_link_forget(LzVM *vm, LzLink *link) {
    LzObject *target = link->target;
    if (target == NULL || vm->link_bucket_count == 0) {
        link->next = NULL;
        return;
    }
    size_t bucket = link_hash(target, vm->link_bucket_count);
    LzLinkRow **at = &vm->link_buckets[bucket];
    while (*at != NULL && (*at)->target != target) {
        at = &(*at)->next;
    }
    LzLinkRow *row = *at;
    if (row == NULL) {
        link->next = NULL;
        return;
    }
    bool found = false;
    for (LzLink **cell = &row->head; *cell != NULL; cell = &(*cell)->next) {
        if (*cell == link) {
            *cell = link->next;
            row->count--;
            found = true;
            break;
        }
    }
    link->next = NULL;
    if (!found) {
        return;
    }
    if (vm->link_count != 0) {
        vm->link_count--;
    }
    if (row->count != 0) {
        return;
    }
    /* The last cell of this target is gone: drop the row and the (live) target's bit. */
    *at = row->next;
    free(row);
    if (vm->link_targets != 0) {
        vm->link_targets--;
    }
    target->linked = 0;
}

LzLink *lz_link_cell_of(LzVM *vm, const LzObject *obj) {
    LzLinkRow *row = row_find(vm, obj);
    return row != NULL ? row->head : NULL;
}

void lz_link_target_freed(LzVM *vm, LzObject *object) {
    if (!object->linked) {
        return;
    }
    LzLinkRow *row = row_find(vm, object);
    object->linked = 0;
    if (row == NULL) {
        return;
    }
    for (LzLink *link = row->head; link != NULL; link = link->next) {
        link->target = NULL;
        vm->link_cancelled++;
    }
    vm->link_count -= row->count;
    if (vm->link_targets != 0) {
        vm->link_targets--;
    }
    if (vm->link_bucket_count == 0) {
        return;
    }
    size_t bucket = link_hash(object, vm->link_bucket_count);
    LzLinkRow **at = &vm->link_buckets[bucket];
    while (*at != NULL && *at != row) {
        at = &(*at)->next;
    }
    if (*at == row) {
        *at = row->next;
    }
    free(row);
}

/*
 * Called by lz_vm_free after lz_heap_destroy, which freed every remaining object without
 * emptying the cells: cells and targets are dangling by now. This function therefore frees rows
 * and the bucket array only and must never read a row's target or a cell.
 */
void lz_link_table_free(LzVM *vm) {
    for (size_t i = 0; i < vm->link_bucket_count; i++) {
        LzLinkRow *row = vm->link_buckets[i];
        while (row != NULL) {
            LzLinkRow *next = row->next;
            free(row);
            row = next;
        }
    }
    free(vm->link_buckets);
    vm->link_buckets = NULL;
    vm->link_bucket_count = 0;
    vm->link_count = 0;
    vm->link_targets = 0;
    vm->link_cancelled = 0;
}

LzStatus lz_vm_link_error(LzVM *vm) {
    return lz_vm_set_error(vm, LZ_ERR_TYPE, "a link has to be read with .target first, got link");
}

LzValue lz_link_deref(const LzLink *link) {
    if (link->target == NULL) {
        return lz_value_nil();
    }
    LzValue value = lz_value_object(link->target);
    lz_value_retain(&value);
    return value;
}

/* ---- the invariant checker (tests and fuzzing; O(heap + table)) ---- */

static int pointer_cmp(const void *a, const void *b) {
    const void *x = *(const void *const *)a;
    const void *y = *(const void *const *)b;
    return x < y ? -1 : (x > y ? 1 : 0);
}

static bool in_sorted(const void *const *items, size_t count, const void *needle) {
    return count != 0 && bsearch(&needle, items, count, sizeof *items, pointer_cmp) != NULL;
}

#define LINK_FAIL(...)                                                                             \
    do {                                                                                           \
        fprintf(stderr, "lz_link_check: " __VA_ARGS__);                                            \
        fputc('\n', stderr);                                                                       \
        free(heap_ptrs);                                                                           \
        return false;                                                                              \
    } while (0)

bool lz_link_check(const LzVM *vm) {
    size_t heap_count = 0;
    for (const LzObject *o = vm->heap.head; o != NULL; o = o->next) {
        heap_count++;
    }
    const void **heap_ptrs = malloc((heap_count != 0 ? heap_count : 1) * sizeof *heap_ptrs);
    if (heap_ptrs == NULL) {
        return true; /* cannot check; not a violation */
    }
    size_t at = 0;
    for (const LzObject *o = vm->heap.head; o != NULL; o = o->next) {
        heap_ptrs[at++] = o;
    }
    qsort(heap_ptrs, heap_count, sizeof *heap_ptrs, pointer_cmp);

    size_t rows = 0;
    size_t cells = 0;
    for (size_t b = 0; b < vm->link_bucket_count; b++) {
        for (const LzLinkRow *row = vm->link_buckets[b]; row != NULL; row = row->next) {
            rows++;
            if (!in_sorted(heap_ptrs, heap_count, row->target)) {
                LINK_FAIL("row for %p: target is not on the heap (freed?)", (const void *)row->target);
            }
            if (row->target->linked != 1) {
                LINK_FAIL("row for %p: target->linked is %u", (const void *)row->target,
                          (unsigned)row->target->linked);
            }
            if (link_hash(row->target, vm->link_bucket_count) != b) {
                LINK_FAIL("row for %p is in the wrong bucket", (const void *)row->target);
            }
            size_t chain = 0;
            for (const LzLink *cell = row->head; cell != NULL; cell = cell->next) {
                chain++;
                if (chain > row->count + 1) {
                    LINK_FAIL("row for %p: chain longer than count %zu", (const void *)row->target,
                              row->count);
                }
                if (!in_sorted(heap_ptrs, heap_count, cell)) {
                    LINK_FAIL("row for %p: a cell is not on the heap", (const void *)row->target);
                }
                if (cell->base.type != LZ_OBJ_LINK) {
                    LINK_FAIL("row for %p: a cell has type %d", (const void *)row->target,
                              (int)cell->base.type);
                }
                if (cell->target != row->target) {
                    LINK_FAIL("row for %p: a cell points at %p", (const void *)row->target,
                              (const void *)cell->target);
                }
            }
            if (chain != row->count) {
                LINK_FAIL("row for %p: count %zu, chain %zu", (const void *)row->target, row->count,
                          chain);
            }
            cells += chain;
        }
    }
    if (cells != vm->link_count) {
        LINK_FAIL("table holds %zu cells, link_count is %zu", cells, vm->link_count);
    }
    if (rows != vm->link_targets) {
        LINK_FAIL("table holds %zu rows, link_targets is %zu", rows, vm->link_targets);
    }
    for (const LzObject *o = vm->heap.head; o != NULL; o = o->next) {
        if (o->linked == 1 && row_find(vm, o) == NULL) {
            LINK_FAIL("object %p (type %d) is linked but has no row", (const void *)o, (int)o->type);
        }
        if (o->linked > 1) {
            LINK_FAIL("object %p has linked == %u", (const void *)o, (unsigned)o->linked);
        }
        if (o->type == LZ_OBJ_LINK) {
            const LzLink *cell = (const LzLink *)o;
            if (cell->target == NULL) {
                continue;
            }
            const LzLinkRow *row = row_find(vm, cell->target);
            bool listed = false;
            for (const LzLink *c = row != NULL ? row->head : NULL; c != NULL; c = c->next) {
                if (c == cell) {
                    listed = true;
                    break;
                }
            }
            if (!listed) {
                LINK_FAIL("cell %p points at %p but is not registered", (const void *)cell,
                          (const void *)cell->target);
            }
        }
    }
    free(heap_ptrs);
    return true;
}
