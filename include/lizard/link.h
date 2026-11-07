#ifndef LIZARD_LINK_H
#define LIZARD_LINK_H

#include "lizard/value.h"

struct LzVM;

/*
 * A link: a non-owning reference to one heap object. A link cell is a heap object like any
 * other, but it never retains its target, so the target can be freed while the link is alive;
 * the VM's link table then empties the cell (see lz_link_target_freed). A link field still
 * works as if it were the object when it is read: `a.parent` yields the target (or nil).
 *
 * link(obj) builds a cell. link(link(x)) returns link(x) itself: a link is never linked.
 */
typedef struct LzLink {
    LzObject base;
    LzObject *target; /* weak: NULL once the target was freed */
    struct LzLink *next; /* the next cell of the same target in the table's row */
} LzLink;

/* One row of the link table: the cells that point at one live target, chained through `next`. */
typedef struct LzLinkRow {
    LzObject *target;
    LzLink *head;
    size_t count;
    struct LzLinkRow *next; /* the next row in the same hash bucket */
} LzLinkRow;

static inline bool lz_value_is_link(const LzValue *value) {
    return value->type == LZ_VAL_OBJECT && value->as.object->type == LZ_OBJ_LINK;
}

static inline LzLink *lz_value_as_link(const LzValue *value) {
    return (LzLink *)value->as.object;
}

/*
 * A cell pointing at target. It does not retain the target: the caller must keep it alive while
 * the cell lives (every path that frees a target first empties the cells, see the table below).
 * The cell is not in the table; call lz_link_track for a non-NULL target.
 */
LzLink *lz_link_new(LzHeap *heap, LzObject *target);

/* True for a kind link() accepts: an instance, an array, a dict, a closure, a function, nil. */
bool lz_link_target_ok(const LzValue *value);

/*
 * Adds a fresh cell to the table, so its target's release can find it. The object's `linked`
 * bit caches whether the table has any row for it.
 */
void lz_link_track(LzVM *vm, LzLink *link);

/* The first cell of obj in the table, or NULL. Only meaningful while obj is alive. */
LzLink *lz_link_cell_of(LzVM *vm, const LzObject *obj);

/* Empties every cell that points at object, called from lz_object_release before the memory goes. */
void lz_link_target_freed(LzVM *vm, LzObject *object);

/* The heap's on_link_freed hook: context is the VM. */
void lz_link_release_hook(void *context, LzObject *object);

/* Removes a dying cell from the table; called by lz_object_finalize. */
void lz_link_forget(LzVM *vm, LzLink *link);

void lz_link_table_free(LzVM *vm);

/* The target of a link cell, or nil. *result is a new reference. */
LzValue lz_link_deref(const LzLink *link);

#endif
