#include "lizard/link.h"

#include "lizard/memory.h"
#include "lizard/vm.h"

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

void lz_link_track(LzVM *vm, LzLink *link) {
    LzObject *target = link->target;
    if (target == NULL) {
        return;
    }
    if (vm->link_count >= vm->link_bucket_count * 2) {
        table_grow(vm);
    }
    LzLinkRow *row = row_find(vm, target);
    if (row == NULL) {
        row = malloc(sizeof *row);
        if (row == NULL) {
            return; /* nothing to remember: the cell stays, it just cannot be emptied early */
        }
        row->target = target;
        row->head = NULL;
        row->count = 0;
        if (vm->link_bucket_count == 0) {
            table_grow(vm);
            if (vm->link_bucket_count == 0) {
                free(row);
                return;
            }
        }
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
}

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
        return;
    }
    for (LzLink **cell = &row->head; *cell != NULL; cell = &(*cell)->next) {
        if (*cell == link) {
            *cell = link->next;
            row->count--;
            break;
        }
    }
    link->next = NULL;
    if (vm->link_count != 0) {
        vm->link_count--;
    }
    if (row->count != 0) {
        return;
    }
    /* The last cell of this target is gone: drop the row (and the object's bit, if it still lives). */
    *at = row->next;
    free(row);
    if (vm->link_targets != 0) {
        vm->link_targets--;
    }
    if (target->linked && row_find(vm, target) == NULL) {
        target->linked = 0;
    }
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

LzValue lz_link_deref(const LzLink *link) {
    if (link->target == NULL) {
        return lz_value_nil();
    }
    LzValue value = lz_value_object(link->target);
    lz_value_retain(&value);
    return value;
}
