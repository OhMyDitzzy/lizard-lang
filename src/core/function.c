#include "lizard/function.h"

#include <stdlib.h>
#include <string.h>

LzFunction *lz_function_new(LzHeap *heap, LzString *name) {
    LzFunction *function = lz_heap_alloc(heap, LZ_OBJ_FUNCTION, sizeof *function);
    if (function == NULL) {
        return NULL;
    }
    function->name = name;
    if (name != NULL) {
        lz_object_retain(&name->base);
    }
    lz_chunk_init(&function->chunk);
    function->param_count = 0;
    function->required = 0;
    function->has_rest = false;
    function->upvalue_count = 0;
    function->upvalues = NULL;
    function->param_names = NULL;
    function->offset = LZ_NO_OFFSET;
    function->engine_cache = NULL;
    function->engine_cache_free = NULL;
    return function;
}

/* Meant to be called once, on a function that has no parameters yet. */
bool lz_function_set_params(LzFunction *function, LzString *const *names, size_t param_count,
                            size_t required, bool has_rest) {
    size_t slots = param_count + (has_rest ? 1u : 0u);
    LzString **copy = NULL;
    if (slots > 0) {
        copy = malloc(slots * sizeof *copy);
        if (copy == NULL) {
            return false;
        }
        for (size_t i = 0; i < slots; i++) {
            copy[i] = names[i];
            lz_object_retain(&names[i]->base);
        }
    }
    function->param_names = copy;
    function->param_count = (uint16_t)param_count;
    function->required = (uint16_t)required;
    function->has_rest = has_rest;
    return true;
}

bool lz_function_set_upvalues(LzFunction *function, const LzUpvalueDesc *descs, size_t count) {
    LzUpvalueDesc *copy = NULL;
    if (count > 0) {
        copy = malloc(count * sizeof *copy);
        if (copy == NULL) {
            return false;
        }
        memcpy(copy, descs, count * sizeof *copy);
    }
    free(function->upvalues);
    function->upvalues = copy;
    function->upvalue_count = (uint16_t)count;
    return true;
}

LzClosure *lz_closure_new(LzHeap *heap, LzFunction *function) {
    size_t size = sizeof(LzClosure) + (size_t)function->upvalue_count * sizeof(LzUpvalue *);
    LzClosure *closure = lz_heap_alloc(heap, LZ_OBJ_CLOSURE, size);
    if (closure == NULL) {
        return NULL;
    }
    lz_object_retain(&function->base);
    closure->function = function;
    closure->upvalue_count = function->upvalue_count;
    for (size_t i = 0; i < closure->upvalue_count; i++) {
        closure->upvalues[i] = NULL;
    }
    return closure;
}

LzUpvalue *lz_upvalue_new(LzHeap *heap, LzValue *slot) {
    LzUpvalue *upvalue = lz_heap_alloc(heap, LZ_OBJ_UPVALUE, sizeof *upvalue);
    if (upvalue == NULL) {
        return NULL;
    }
    upvalue->location = slot;
    upvalue->closed = lz_value_nil();
    upvalue->next_open = NULL;
    upvalue->watch.prev = NULL;
    upvalue->watch.next = NULL;
    return upvalue;
}

const char *lz_function_display_name(const LzFunction *function) {
    return function->name != NULL ? function->name->data : "<fn>";
}

void lz_function_free_storage(LzHeap *heap, LzFunction *function, bool release_refs) {
    if (function->engine_cache != NULL && function->engine_cache_free != NULL) {
        function->engine_cache_free(function->engine_cache);
    }
    function->engine_cache = NULL;
    if (release_refs) {
        lz_chunk_free(heap, &function->chunk);
        if (function->name != NULL) {
            lz_object_release(heap, &function->name->base);
        }
        size_t slots = lz_function_slots(function);
        for (size_t i = 0; function->param_names != NULL && i < slots; i++) {
            lz_object_release(heap, &function->param_names[i]->base);
        }
    } else {
        lz_chunk_discard(&function->chunk);
    }
    free(function->param_names);
    free(function->upvalues);
    function->param_names = NULL;
    function->upvalues = NULL;
}
