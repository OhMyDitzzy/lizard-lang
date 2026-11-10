#define _GNU_SOURCE 1
#include <stdint.h>
#include <stdio.h>
#if defined(__linux__)
#include <pthread.h>
#elif defined(__APPLE__)
#include <pthread.h>
#endif
#include <string.h>

#include "lizard/array.h"
#include "lizard/class.h"
#include "lizard/link.h"
#include "lizard/native.h"
#include "lizard/suggest.h"
#include "lizard/vm.h"

/*
 * Calls, frames and upvalues. Everything an engine needs to run a user function that is not
 * "execute the next instruction" lives here, so the reference interpreter and the Gecko Engine
 * can't disagree about what a call means.
 */

static LzStatus malformed(LzVM *vm, const char *what) {
    return lz_vm_set_error(vm, LZ_ERR_RUNTIME, "malformed bytecode: %s", what);
}

static LzStatus not_callable_error(LzVM *vm) {
    LzStatus status = lz_vm_set_error(vm, LZ_ERR_RUNTIME, "can only call functions");
    lz_vm_set_help(vm, "only functions and classes can be called; did you forget an operator?");
    return status;
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

/*
 * "signature: f(a, b = ..., *rest)" lists the parameters of `function`, for a call that did not
 * fit.
 */
static void signature_help(LzVM *vm, const LzFunction *function) {
    char text[256];
    size_t used = (size_t)snprintf(text, sizeof text, "signature: %s(", lz_function_display_name(function));
    for (size_t i = 0; i < lz_function_slots(function) && used < sizeof text; i++) {
        bool rest = i >= function->param_count;
        used += (size_t)snprintf(text + used, sizeof text - used, "%s%s%s%s", i == 0 ? "" : ", ",
                                 rest ? "*" : "", function->param_names[i]->data,
                                 !rest && i >= function->required ? " = ..." : "");
    }
    if (used < sizeof text) {
        (void)snprintf(text + used, sizeof text - used, ")");
    }
    lz_vm_set_help(vm, "%s", text);
}

static LzStatus arity_error(LzVM *vm, const LzFunction *function, size_t given) {
    const char *name = lz_function_display_name(function);
    size_t required = function->required;
    size_t most = function->param_count;
    LzStatus status;
    if (function->has_rest) {
        status = lz_vm_set_error(vm, LZ_ERR_ARITY, "%s(): expected at least %zu argument%s, got %zu",
                                 name, required, plural(required), given);
    } else if (required == most) {
        status = lz_vm_set_error(vm, LZ_ERR_ARITY, "%s(): expected %zu argument%s, got %zu", name,
                                 required, plural(required), given);
    } else {
        status = lz_vm_set_error(vm, LZ_ERR_ARITY, "%s(): expected %zu to %zu arguments, got %zu", name,
                                 required, most, given);
    }
    signature_help(vm, function);
    return status;
}

/* Bit i is set for every parameter below argc: the arguments the caller passed by position. */
static uint64_t supplied_mask(size_t argc) {
    return argc >= 64 ? LZ_ALL_SUPPLIED : (((uint64_t)1 << argc) - 1u);
}

static LzStatus push_frame(LzVM *vm, LzClosure *closure, size_t base, uint64_t supplied,
                           size_t resume_ip, uint32_t call_offset) {
    if (vm->frame_count >= LZ_FRAMES_MAX) {
        LzStatus status = lz_vm_set_error(vm, LZ_ERR_STACK_OVERFLOW,
                                          "stack overflow: too many nested calls (limit %d)", LZ_FRAMES_MAX);
        lz_vm_set_help(vm, "a function calls itself without a base case, or the recursion is too deep (limit %d)",
                       LZ_FRAMES_MAX);
        return status;
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
 * Turns the argc arguments on top of the stack into the parameter slots of function. Missing
 * ones become nil, and their defaults run in the callee's prologue. With a rest parameter the
 * surplus is collected into a new array, which is empty when there's no surplus. With strict,
 * too few arguments are an error as well, but named arguments may still fill in the required
 * ones, so that check waits for them.
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


/*
 * A bound method becomes its function with the receiver as the first argument, or just the
 * function when it drops the first argument. This repeats while the result is another bound
 * method.
 */
static LzStatus unwrap_bound(LzVM *vm, size_t callee_index, size_t *argc) {
    while (lz_value_is_bound(&vm->stack[callee_index])) {
        LzBound *bound = (LzBound *)vm->stack[callee_index].as.object;
        LzValue callee = bound->callee;
        lz_value_retain(&callee);
        if (bound->skip_first) {
            if (*argc == 0) {
                lz_value_release(&vm->heap, &callee);
                return not_callable_error(vm);
            }
            LzValue first = vm->stack[callee_index + 1];
            memmove(&vm->stack[callee_index + 1], &vm->stack[callee_index + 2],
                    (*argc - 1) * sizeof(LzValue));
            vm->stack_count--;
            (*argc)--;
            lz_value_release(&vm->heap, &first);
        } else {
            if (vm->stack_count + 1 > LZ_STACK_MAX) {
                lz_value_release(&vm->heap, &callee);
                return stack_overflow(vm);
            }
            LzValue receiver = bound->receiver;
            lz_value_retain(&receiver);
            memmove(&vm->stack[callee_index + 2], &vm->stack[callee_index + 1], *argc * sizeof(LzValue));
            vm->stack[callee_index + 1] = receiver;
            vm->stack_count++;
            (*argc)++;
        }
        LzValue old = vm->stack[callee_index];
        vm->stack[callee_index] = callee;
        lz_value_release(&vm->heap, &old);
    }
    return LZ_OK;
}

/* Calls a class: the arguments and the callee on the stack are replaced by the new object. */
static LzStatus construct_on_stack(LzVM *vm, size_t callee_index, size_t argc, size_t named_count,
                                   const char *names, size_t names_length, const LzValue *named) {
    LzClass *cls = lz_value_as_class(&vm->stack[callee_index]);
    LzValue made = lz_value_nil();
    LzStatus status = lz_class_construct(vm, cls, argc, &vm->stack[callee_index + 1], named_count, names,
                                         names_length, named, &made);
    if (status != LZ_OK) {
        return status;
    }
    for (size_t i = 0; i < argc + 1; i++) {
        drop_top(vm);
    }
    vm->stack[vm->stack_count++] = made;
    return LZ_OK;
}

LzStatus lz_vm_call(LzVM *vm, size_t argc, size_t resume_ip, uint32_t call_offset, bool *entered) {
    *entered = false;
    if (vm->stack_count < argc + 1) {
        return malformed(vm, "stack underflow");
    }

    size_t callee_index = vm->stack_count - argc - 1;
    if (lz_value_is_bound(&vm->stack[callee_index])) {
        LzStatus unwrapped = unwrap_bound(vm, callee_index, &argc);
        if (unwrapped != LZ_OK) {
            return unwrapped;
        }
    }
    const LzValue *callee = &vm->stack[callee_index];
    if (lz_value_is_class(callee)) {
        vm->native_call_offset = call_offset;
        return construct_on_stack(vm, callee_index, argc, 0, NULL, 0, NULL);
    }
    if (is_native(callee)) {
        vm->native_call_offset = call_offset;
        return call_native(vm, (const LzNative *)callee->as.object, argc);
    }
    if (!lz_value_is_closure(callee)) {
        return lz_value_is_link(callee) ? lz_vm_link_error(vm)
                                        : not_callable_error(vm);
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

/*
 * The lowest address the nested callbacks may reach. A callback costs 1 to 3 KiB of C stack,
 * more under AddressSanitizer, so the count limit alone can't protect a small stack. The real
 * end of this thread's stack is asked of the system and 192 KiB are kept back for the work
 * between two checks. Where it can't be asked, the callbacks get 768 KiB below the first one.
 */
#define C_STACK_MARGIN (192u * 1024u)
#define C_STACK_FALLBACK (768u * 1024u)

/*
 * Asking the system for the stack of a thread is slow, and on Linux it reads /proc/self/maps. So
 * the answer is kept in the VM and used again while the code runs on the same stack.
 */
static uintptr_t c_stack_floor(LzVM *vm, uintptr_t now) {
    if (vm->c_stack_hi != 0 && now >= vm->c_stack_lo && now < vm->c_stack_hi) {
        return vm->c_stack_lo + C_STACK_MARGIN;
    }
#if defined(__linux__)
    pthread_attr_t attr;
    if (pthread_getattr_np(pthread_self(), &attr) == 0) {
        void *low = NULL;
        size_t size = 0;
        int ok = pthread_attr_getstack(&attr, &low, &size);
        (void)pthread_attr_destroy(&attr);
        if (ok == 0 && low != NULL && (uintptr_t)low < now && now - (uintptr_t)low > C_STACK_MARGIN) {
            vm->c_stack_lo = (uintptr_t)low;
            vm->c_stack_hi = (uintptr_t)low + size;
            return vm->c_stack_lo + C_STACK_MARGIN;
        }
    }
#elif defined(__APPLE__)
    pthread_t self = pthread_self();
    uintptr_t top = (uintptr_t)pthread_get_stackaddr_np(self);
    size_t size = pthread_get_stacksize_np(self);
    if (top > now && size > C_STACK_MARGIN && top - size < now && now - (top - size) > C_STACK_MARGIN) {
        vm->c_stack_lo = top - size;
        vm->c_stack_hi = top;
        return vm->c_stack_lo + C_STACK_MARGIN;
    }
#endif
    return now > C_STACK_FALLBACK ? now - C_STACK_FALLBACK : 0;
}

/*
 * The address of this frame on the real stack. The address of a local would be wrong under
 * AddressSanitizer, which moves locals to a "fake stack" on the heap.
 */
#if defined(__GNUC__) || defined(__clang__)
#define C_STACK_NOW() ((uintptr_t)__builtin_frame_address(0))
#else
#define C_STACK_NOW() ((uintptr_t)&here)
#endif

/* True when this callback would nest deeper than the C stack can take. */
static bool c_stack_exhausted(LzVM *vm) {
    char here;
    (void)here;
    uintptr_t now = C_STACK_NOW();
    if (vm->native_depth == 0) {
        vm->c_stack_floor = c_stack_floor(vm, now);
        return false;
    }
    return now < vm->c_stack_floor;
}

LzStatus lz_vm_call_value(LzVM *vm, const LzValue *callee, size_t argc, const LzValue *argv,
                          LzValue *result) {
    if (c_stack_exhausted(vm)) {
        return lz_vm_set_error(vm, LZ_ERR_STACK_OVERFLOW,
                               "stack overflow: callbacks nested too deeply for the C stack");
    }
    if (vm->native_depth >= LZ_NATIVE_DEPTH_MAX) {
        return lz_vm_set_error(vm, LZ_ERR_STACK_OVERFLOW,
                               "stack overflow: callbacks nested too deeply (limit %d)",
                               LZ_NATIVE_DEPTH_MAX);
    }
    if (vm->frame_count == 0 || vm->stack_count + argc + 1 > LZ_STACK_MAX) {
        return stack_overflow(vm);
    }

    /*
     * Another callback may run between two calls of this native, so put the call site back
     * after.
     */
    uint32_t call_site = vm->native_call_offset;
    size_t base = vm->stack_count;
    lz_value_retain(callee);
    vm->stack[vm->stack_count++] = *callee;
    for (size_t i = 0; i < argc; i++) {
        LzValue arg = argv[i];
        lz_value_retain(&arg);
        vm->stack[vm->stack_count++] = arg;
    }

    size_t depth = vm->frame_count;
    size_t resume = vm->frames[depth - 1].ip; /* the caller's own place is not changed */
    bool entered = false;
    vm->native_depth++;
    LzStatus status = lz_vm_call(vm, argc, resume, call_site, &entered);
    if (status == LZ_OK && entered) {
        size_t failed_at = 0;
        status = lz_vm_run_until(vm, depth, &failed_at);
        if (status != LZ_OK && vm->error_offset == LZ_NO_OFFSET) {
            /*
             * The innermost failure wins, because the engine would otherwise read this offset
             * from the wrong chunk.
             */
            const LzChunk *failing = vm->frames[vm->frame_count - 1].chunk;
            if (failed_at < failing->count) {
                vm->error_offset = failing->offsets[failed_at];
            }
        }
    }
    vm->native_depth--;
    vm->native_call_offset = call_site;
    if (status != LZ_OK) {
        if (!vm->trace_fixed) {
            /*
             * Record the stack while the callback's frames still exist, then take them away,
             * because the engine that called the native expects the frame it ran in on top.
             */
            lz_vm_capture_trace(vm);
            vm->trace_fixed = true;
        }
        vm->frame_count = depth;
        while (vm->handler_count > 0 && vm->handlers[vm->handler_count - 1].frame >= depth) {
            vm->handler_count--; /* the frames they belonged to are gone */
        }
        lz_vm_close_upvalues(vm, base);
        while (vm->stack_count > base) {
            drop_top(vm);
        }
        return status;
    }
    if (vm->stack_count != base + 1) {
        return malformed(vm, "a callback left the stack unbalanced");
    }
    *result = vm->stack[--vm->stack_count]; /* the stack's reference moves to the caller */
    return LZ_OK;
}

static void drop_values(LzVM *vm, LzValue *values, size_t count) {
    for (size_t i = 0; i < count; i++) {
        lz_value_release(&vm->heap, &values[i]);
    }
}

/* The parameter called name[0..length), or -1. The rest parameter can't be passed by name. */
static int find_param(const LzFunction *function, const char *name, size_t length) {
    for (size_t i = 0; i < function->param_count; i++) {
        const LzString *candidate = function->param_names[i];
        if (candidate->length == length && memcmp(candidate->data, name, length) == 0) {
            return (int)i;
        }
    }
    return -1;
}

/*
 * A named argument that is no parameter. The error gives the closest name, then the names there
 * are.
 */
static void unknown_argument_help(LzVM *vm, const LzFunction *function, const char *name, size_t length) {
    char text[300];
    size_t used = 0;
    LzSuggester suggester;
    lz_suggester_init(&suggester, name, length);
    for (size_t i = 0; i < function->param_count; i++) {
        lz_suggester_offer(&suggester, function->param_names[i]->data);
    }
    if (suggester.found.count > 0) {
        used += (size_t)snprintf(text, sizeof text, "did you mean `%s`? ", suggester.found.names[0]);
    }
    used += (size_t)snprintf(text + used, sizeof text - used, "the parameters are: ");
    for (size_t i = 0; i < function->param_count && used < sizeof text; i++) {
        used += (size_t)snprintf(text + used, sizeof text - used, "%s%s", i == 0 ? "" : ", ",
                                 function->param_names[i]->data);
    }
    if (function->param_count == 0) {
        (void)snprintf(text + used, sizeof text - used, "none");
    }
    lz_vm_set_help(vm, "%s", text);
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
    bool wrapped = lz_value_is_bound(callee) || lz_value_is_class(callee);
    if (!native && !wrapped && !lz_value_is_closure(callee)) {
        if (lz_value_is_link(callee)) {
            return lz_vm_link_error(vm);
        }
        return not_callable_error(vm);
    }
    uint8_t error_kind = 0;
    bool makes_error = native && lz_native_error_kind((const LzNative *)callee->as.object, &error_kind);
    bool is_with = native && callee->as.object == vm->native_with;
    if (native && named_count > 0 && !makes_error && !is_with) {
        return lz_vm_set_error(vm, LZ_ERR_ARITY, "%s(): builtin functions do not take named arguments",
                               ((const LzNative *)callee->as.object)->name->data);
    }

    /* Take the named values off the stack. From here on every exit must release them. */
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

    if (lz_value_is_bound(&vm->stack[callee_index])) {
        status = unwrap_bound(vm, callee_index, &count);
        if (status != LZ_OK) {
            drop_values(vm, named, named_count);
            return status;
        }
        callee = &vm->stack[callee_index];
        if (is_native(callee) || (!lz_value_is_closure(callee) && !lz_value_is_class(callee))) {
            drop_values(vm, named, named_count);
            if (is_native(callee)) {
                return lz_vm_set_error(vm, LZ_ERR_ARITY, "%s(): builtin functions do not take named arguments",
                                       ((const LzNative *)callee->as.object)->name->data);
            }
            return not_callable_error(vm);
        }
    }
    if (lz_value_is_class(&vm->stack[callee_index])) {
        vm->native_call_offset = call_offset;
        status = construct_on_stack(vm, callee_index, count, named_count, names->data, names->length, named);
        drop_values(vm, named, named_count);
        return status;
    }
    if (is_with) {
        vm->native_call_offset = call_offset;
        LzValue made = lz_value_nil();
        bool on_error = lz_value_is_error(&vm->stack[callee_index + 1]);
        if (count != 1 || (!on_error && !lz_value_is_instance(&vm->stack[callee_index + 1]))) {
            drop_values(vm, named, named_count);
            return lz_vm_set_error(vm, LZ_ERR_ARITY,
                                   on_error ? "with() takes named fields, like e.with(needed: 5)"
                                            : "with() takes named fields, like p.with(x: 10)");
        }
        if (on_error) {
            status = lz_error_with(vm, (const LzError *)vm->stack[callee_index + 1].as.object, named_count,
                                   names->data, names->length, named, &made);
        } else {
            status = lz_instance_with(vm, lz_value_as_instance(&vm->stack[callee_index + 1]), named_count,
                                      names->data, names->length, named, &made);
        }
        drop_values(vm, named, named_count);
        if (status != LZ_OK) {
            return status;
        }
        for (size_t i = 0; i < count + 1; i++) {
            drop_top(vm);
        }
        vm->stack[vm->stack_count++] = made;
        return LZ_OK;
    }

    if (makes_error) {
        /* `Kind("message", cause: e, data: d)`, where the named values are what the error keeps. */
        vm->native_call_offset = call_offset;
        LzValue made = lz_value_nil();
        status = lz_error_construct_named(vm, error_kind, count, &vm->stack[callee_index + 1],
                                          named_count, names->data, names->length, named, &made);
        drop_values(vm, named, named_count);
        if (status != LZ_OK) {
            return status;
        }
        for (size_t i = 0; i < count + 1; i++) {
            drop_top(vm);
        }
        vm->stack[vm->stack_count++] = made;
        return LZ_OK;
    }

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
            unknown_argument_help(vm, function, names->data + position, end - position);
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
            signature_help(vm, function);
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
    /* A try left open in this frame, from a `return` that skipped its end, dies with the frame. */
    while (vm->handler_count > 0 && vm->handlers[vm->handler_count - 1].frame >= vm->frame_count - 1) {
        vm->handler_count--;
    }
    lz_vm_close_upvalues(vm, base);
    while (vm->stack_count > base - 1) {
        drop_top(vm); /* locals, arguments and the callee itself */
    }
    vm->stack[vm->stack_count++] = result;
    vm->frame_count--;
    lz_skink_run(vm); /* the frame's slots are gone, so cycles it left behind are dead now */
    return LZ_OK;
}

/* The open upvalue for this slot, created and added to the VM's list when there is none. */
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
        lz_value_retain(upvalue->location); /* the stack slot is about to go, but this copy stays */
        upvalue->closed = *upvalue->location;
        upvalue->location = &upvalue->closed;
        vm->open_upvalues = upvalue->next_open;
        upvalue->next_open = NULL;
        lz_skink_note_closed(vm, upvalue); /* before the list lets go, because that may be the last reference */
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
