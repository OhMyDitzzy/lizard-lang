#include <stdio.h>
#include <string.h>

#include "lizard/array.h"
#include "lizard/native.h"
#include "lizard/vm.h"

/*
 * Calls, frames and upvalues. Everything an engine needs to run a user function
 * that is not "execute the next instruction" lives here, so the reference
 * interpreter and the Gecko Engine cannot disagree about what a call means.
 */

static LzStatus malformed(LzVM *vm, const char *what) {
    return lz_vm_set_error(vm, LZ_ERR_RUNTIME, "malformed bytecode: %s", what);
}

static LzStatus stack_overflow(LzVM *vm) {
    return lz_vm_set_error(vm, LZ_ERR_STACK_OVERFLOW, "stack overflow");
}

static void drop_top(LzVM *vm) {
    vm->stack_count--;
    lz_value_release(&vm->heap, &vm->stack[vm->stack_count]);
}

static bool is_native(const LzValue *value) {
    return value->type == LZ_VAL_OBJECT && value->as.object->type == LZ_OBJ_NATIVE;
}

static const char *plural(size_t n) {
    return n == 1 ? "" : "s";
}

static LzStatus arity_error(LzVM *vm, const LzFunction *function, size_t given) {
    const char *name = lz_function_display_name(function);
    size_t required = function->required;
    size_t most = function->param_count;
    if (function->has_rest) {
        return lz_vm_set_error(vm, LZ_ERR_ARITY, "%s(): expected at least %zu argument%s, got %zu",
                               name, required, plural(required), given);
    }
    if (required == most) {
        return lz_vm_set_error(vm, LZ_ERR_ARITY, "%s(): expected %zu argument%s, got %zu", name,
                               required, plural(required), given);
    }
    return lz_vm_set_error(vm, LZ_ERR_ARITY, "%s(): expected %zu to %zu arguments, got %zu", name,
                           required, most, given);
}

/* Bit i is set for every parameter below argc: the arguments the caller passed by position. */
static uint64_t supplied_mask(size_t argc) {
    return argc >= 64 ? LZ_ALL_SUPPLIED : (((uint64_t)1 << argc) - 1u);
}

static LzStatus push_frame(LzVM *vm, LzClosure *closure, size_t base, uint64_t supplied,
                           size_t resume_ip, uint32_t call_offset) {
    if (vm->frame_count >= LZ_FRAMES_MAX) {
        return lz_vm_set_error(vm, LZ_ERR_STACK_OVERFLOW,
                               "stack overflow: too many nested calls (limit %d)", LZ_FRAMES_MAX);
    }
    vm->frames[vm->frame_count - 1].ip = resume_ip;
    LzCallFrame *frame = &vm->frames[vm->frame_count++];
    frame->closure = closure;
    frame->chunk = &closure->function->chunk;
    frame->ip = 0;
    frame->base = base;
    frame->supplied = supplied;
    frame->call_offset = call_offset;
    return LZ_OK;
}

/*
 * Turns the argc arguments on top of the stack into the parameter slots of function:
 * missing ones become nil (their defaults run in the callee's prologue), and with a
 * rest parameter the surplus is collected into a new array (an empty one when there
 * is no surplus). With strict, too few arguments are an error as well; named
 * arguments may still fill in the required ones, so that check waits for them.
 */
static LzStatus arrange(LzVM *vm, const LzFunction *function, size_t argc, bool strict) {
    size_t params = function->param_count;
    if (argc > params && !function->has_rest) {
        return arity_error(vm, function, argc);
    }
    if (strict && argc < function->required) {
        return arity_error(vm, function, argc);
    }

    size_t base = vm->stack_count - argc;
    if (base + lz_function_slots(function) > LZ_STACK_MAX) {
        return stack_overflow(vm);
    }

    if (argc > params) {
        size_t extra = argc - params;
        LzArray *rest = lz_array_new(&vm->heap, extra);
        if (rest == NULL) {
            return lz_vm_out_of_memory(vm);
        }
        for (size_t i = 0; i < extra; i++) {
            LzStatus status = lz_array_push(vm, rest, &vm->stack[base + params + i]);
            if (status != LZ_OK) {
                lz_object_release(&vm->heap, &rest->base);
                return status;
            }
        }
        while (vm->stack_count > base + params) {
            drop_top(vm);
        }
        vm->stack[vm->stack_count++] = lz_value_object(&rest->base); /* keeps the creation reference */
        return LZ_OK;
    }

    for (size_t i = argc; i < params; i++) {
        vm->stack[vm->stack_count++] = lz_value_nil();
    }
    if (function->has_rest) {
        LzArray *rest = lz_array_new(&vm->heap, 0);
        if (rest == NULL) {
            return lz_vm_out_of_memory(vm);
        }
        vm->stack[vm->stack_count++] = lz_value_object(&rest->base);
    }
    return LZ_OK;
}

static LzStatus call_native(LzVM *vm, const LzNative *native, size_t argc) {
    /* The result is a new reference owned by this frame until it is pushed. */
    LzValue result = lz_value_nil();
    const LzValue *args = &vm->stack[vm->stack_count - argc];
    LzStatus status = lz_native_call(vm, native, (int)argc, args, &result);
    if (status != LZ_OK) {
        return status;
    }
    for (size_t i = 0; i < argc + 1; i++) {
        drop_top(vm);
    }
    vm->stack[vm->stack_count++] = result;
    return LZ_OK;
}

LzStatus lz_vm_call(LzVM *vm, size_t argc, size_t resume_ip, uint32_t call_offset, bool *entered) {
    *entered = false;
    if (vm->stack_count < argc + 1) {
        return malformed(vm, "stack underflow");
    }

    size_t callee_index = vm->stack_count - argc - 1;
    const LzValue *callee = &vm->stack[callee_index];
    if (is_native(callee)) {
        return call_native(vm, (const LzNative *)callee->as.object, argc);
    }
    if (!lz_value_is_closure(callee)) {
        return lz_vm_set_error(vm, LZ_ERR_RUNTIME, "can only call functions");
    }

    LzClosure *closure = lz_value_as_closure(callee);
    LzStatus status = arrange(vm, closure->function, argc, true);
    if (status != LZ_OK) {
        return status;
    }
    status = push_frame(vm, closure, callee_index + 1, supplied_mask(argc), resume_ip, call_offset);
    *entered = status == LZ_OK;
    return status;
}

static void drop_values(LzVM *vm, LzValue *values, size_t count) {
    for (size_t i = 0; i < count; i++) {
        lz_value_release(&vm->heap, &values[i]);
    }
}

/* The parameter called name[0..length), or -1. The rest parameter cannot be passed by name. */
static int find_param(const LzFunction *function, const char *name, size_t length) {
    for (size_t i = 0; i < function->param_count; i++) {
        const LzString *candidate = function->param_names[i];
        if (candidate->length == length && memcmp(candidate->data, name, length) == 0) {
            return (int)i;
        }
    }
    return -1;
}

LzStatus lz_vm_call_ex(LzVM *vm, const LzChunk *chunk, size_t named_count, uint16_t names_index,
                       size_t resume_ip, uint32_t call_offset, bool *entered) {
    *entered = false;
    if (vm->stack_count < named_count + 2) {
        return malformed(vm, "stack underflow");
    }
    size_t array_index = vm->stack_count - named_count - 1;
    if (!lz_value_is_array(&vm->stack[array_index])) {
        return malformed(vm, "argument array expected");
    }

    const LzString *names = NULL;
    if (named_count > 0) {
        if (names_index >= chunk->constant_count || chunk->constants[names_index].type != LZ_VAL_OBJECT ||
            chunk->constants[names_index].as.object->type != LZ_OBJ_STRING) {
            return malformed(vm, "argument names are not a string");
        }
        names = (const LzString *)chunk->constants[names_index].as.object;
    }

    size_t callee_index = array_index - 1;
    const LzValue *callee = &vm->stack[callee_index];
    bool native = is_native(callee);
    if (!native && !lz_value_is_closure(callee)) {
        return lz_vm_set_error(vm, LZ_ERR_RUNTIME, "can only call functions");
    }
    if (native && named_count > 0) {
        return lz_vm_set_error(vm, LZ_ERR_ARITY, "%s(): builtin functions do not take named arguments",
                               ((const LzNative *)callee->as.object)->name->data);
    }

    /* Take the named values off the stack; from here on every exit must release them. */
    LzValue named[255];
    for (size_t i = 0; i < named_count; i++) {
        named[i] = vm->stack[array_index + 1 + i];
    }
    vm->stack_count = array_index + 1;

    /* Spread the positional array in place of itself. */
    LzArray *array = lz_value_as_array(&vm->stack[array_index]);
    size_t count = array->count;
    if (array_index + count > LZ_STACK_MAX) {
        drop_values(vm, named, named_count);
        return stack_overflow(vm);
    }
    LzStatus status = lz_vm_charge_fuel(vm, lz_array_cost(count));
    if (status != LZ_OK) {
        drop_values(vm, named, named_count);
        return status;
    }
    LzObject *holder = &array->base; /* the stack slot's reference, released below */
    for (size_t i = 0; i < count; i++) {
        LzValue item = array->items[i];
        lz_value_retain(&item);
        vm->stack[array_index + i] = item;
    }
    vm->stack_count = array_index + count;
    lz_object_release(&vm->heap, holder);

    if (named_count == 0) {
        return lz_vm_call(vm, count, resume_ip, call_offset, entered);
    }

    /* Named arguments: find each one's parameter. */
    LzClosure *closure = lz_value_as_closure(&vm->stack[callee_index]);
    const LzFunction *function = closure->function;
    size_t positional = count < function->param_count ? count : function->param_count;
    uint64_t mask = supplied_mask(positional);
    uint8_t target[255];
    size_t position = 0;
    for (size_t k = 0; k < named_count; k++) {
        if (position > names->length) {
            drop_values(vm, named, named_count);
            return malformed(vm, "missing argument name");
        }
        size_t end = position;
        while (end < names->length && names->data[end] != '\0') {
            end++;
        }
        int index = find_param(function, names->data + position, end - position);
        if (index < 0) {
            status = lz_vm_set_error(vm, LZ_ERR_ARITY, "%s(): unexpected named argument '%.*s'",
                                     lz_function_display_name(function), (int)(end - position),
                                     names->data + position);
            drop_values(vm, named, named_count);
            return status;
        }
        if ((mask >> index) & 1u) {
            status = lz_vm_set_error(vm, LZ_ERR_ARITY, "%s(): got multiple values for argument '%s'",
                                     lz_function_display_name(function), function->param_names[index]->data);
            drop_values(vm, named, named_count);
            return status;
        }
        mask |= (uint64_t)1 << index;
        target[k] = (uint8_t)index;
        position = end + 1;
    }

    if (count > function->param_count && !function->has_rest) {
        status = arity_error(vm, function, count + named_count);
        drop_values(vm, named, named_count);
        return status;
    }
    for (size_t i = 0; i < function->required; i++) {
        if (((mask >> i) & 1u) == 0) {
            status = lz_vm_set_error(vm, LZ_ERR_ARITY, "%s(): missing argument '%s'",
                                     lz_function_display_name(function), function->param_names[i]->data);
            drop_values(vm, named, named_count);
            return status;
        }
    }

    status = arrange(vm, function, count, false);
    if (status != LZ_OK) {
        drop_values(vm, named, named_count);
        return status;
    }
    size_t base = callee_index + 1;
    for (size_t k = 0; k < named_count; k++) {
        vm->stack[base + target[k]] = named[k]; /* the slot held nil: nothing to release */
    }
    status = push_frame(vm, closure, base, mask, resume_ip, call_offset);
    *entered = status == LZ_OK;
    return status;
}

LzStatus lz_vm_return(LzVM *vm) {
    if (vm->frame_count < 2) {
        return malformed(vm, "return outside a function");
    }
    size_t base = vm->frames[vm->frame_count - 1].base;
    if (vm->stack_count <= base) {
        return malformed(vm, "stack underflow");
    }
    LzValue result = vm->stack[--vm->stack_count];
    lz_vm_close_upvalues(vm, base);
    while (vm->stack_count > base - 1) {
        drop_top(vm); /* locals, arguments and the callee itself */
    }
    vm->stack[vm->stack_count++] = result;
    vm->frame_count--;
    lz_skink_run(vm); /* the frame's slots are gone: cycles it left behind are dead now */
    return LZ_OK;
}

/* The open upvalue for this slot, created (and added to the VM's list) when there is none. */
static LzUpvalue *capture_upvalue(LzVM *vm, LzValue *slot) {
    LzUpvalue *previous = NULL;
    LzUpvalue *current = vm->open_upvalues;
    while (current != NULL && current->location > slot) {
        previous = current;
        current = current->next_open;
    }
    if (current != NULL && current->location == slot) {
        return current;
    }

    LzUpvalue *created = lz_upvalue_new(&vm->heap, slot); /* the list owns the creation reference */
    if (created == NULL) {
        return NULL;
    }
    created->next_open = current;
    if (previous == NULL) {
        vm->open_upvalues = created;
    } else {
        previous->next_open = created;
    }
    return created;
}

void lz_vm_close_upvalues(LzVM *vm, size_t level) {
    if (vm->open_upvalues == NULL) {
        return;
    }
    const LzValue *last = &vm->stack[level];
    while (vm->open_upvalues != NULL && vm->open_upvalues->location >= last) {
        LzUpvalue *upvalue = vm->open_upvalues;
        lz_value_retain(upvalue->location); /* the stack slot is about to go; this copy stays */
        upvalue->closed = *upvalue->location;
        upvalue->location = &upvalue->closed;
        vm->open_upvalues = upvalue->next_open;
        upvalue->next_open = NULL;
        lz_skink_note_closed(vm, upvalue); /* before the list lets go: that may be the last reference */
        lz_object_release(&vm->heap, &upvalue->base); /* the list's reference */
    }
}

LzStatus lz_vm_make_closure(LzVM *vm, LzFunction *function) {
    if (vm->stack_count >= LZ_STACK_MAX) {
        return stack_overflow(vm);
    }
    const LzCallFrame *frame = &vm->frames[vm->frame_count - 1];
    LzClosure *closure = lz_closure_new(&vm->heap, function);
    if (closure == NULL) {
        return lz_vm_out_of_memory(vm);
    }

    for (size_t i = 0; i < function->upvalue_count; i++) {
        const LzUpvalueDesc *desc = &function->upvalues[i];
        LzUpvalue *upvalue = NULL;
        if (desc->is_local) {
            size_t slot = frame->base + desc->index;
            if (slot >= vm->stack_count) {
                lz_object_release(&vm->heap, &closure->base);
                return malformed(vm, "captured slot out of range");
            }
            upvalue = capture_upvalue(vm, &vm->stack[slot]);
            if (upvalue == NULL) {
                lz_object_release(&vm->heap, &closure->base);
                return lz_vm_out_of_memory(vm);
            }
        } else {
            if (frame->closure == NULL || desc->index >= frame->closure->upvalue_count) {
                lz_object_release(&vm->heap, &closure->base);
                return malformed(vm, "captured upvalue out of range");
            }
            upvalue = frame->closure->upvalues[desc->index];
        }
        lz_object_retain(&upvalue->base); /* the closure's own reference */
        closure->upvalues[i] = upvalue;
    }

    vm->stack[vm->stack_count++] = lz_value_object(&closure->base);
    return LZ_OK;
}

void lz_vm_capture_trace(LzVM *vm) {
    vm->trace_count = 0;
    vm->trace_total = 0;
    if (vm->frame_count < 2) {
        return; /* a failure in the script itself needs no trace */
    }
    vm->trace_total = vm->frame_count;
    uint32_t offset = vm->error_offset;
    for (size_t i = vm->frame_count; i > 0 && vm->trace_count < LZ_TRACE_MAX; i--) {
        const LzCallFrame *frame = &vm->frames[i - 1];
        LzTraceEntry *entry = &vm->trace[vm->trace_count++];
        const char *name = frame->closure != NULL ? lz_function_display_name(frame->closure->function)
                                                  : "<script>";
        (void)snprintf(entry->name, sizeof entry->name, "%s", name);
        entry->offset = offset;
        /* The next frame down was paused at the call that created this one. */
        offset = LZ_NO_OFFSET;
        if (i >= 2 && frame->call_offset < vm->frames[i - 2].chunk->count) {
            offset = vm->frames[i - 2].chunk->offsets[frame->call_offset];
        }
    }
}
