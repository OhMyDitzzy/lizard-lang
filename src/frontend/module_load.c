#include "lizard/module.h"
#include "lizard/compiler.h"
#include "lizard/vm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static size_t fail_countdown = 0;

void lz_module_fail_after(size_t n) {
    fail_countdown = n;
}

static bool alloc_fails(void) {
    if (fail_countdown == 0) {
        return false;
    }
    return --fail_countdown == 0;
}

static void *mod_calloc(size_t size) {
    return alloc_fails() ? NULL : calloc(1, size);
}

static void source_discard(LzModuleSource *source) {
    free(source->id);
    free(source->name);
    free(source->text);
    source->id = NULL;
    source->name = NULL;
    source->text = NULL;
}

static LzModule *find_module(const LzVM *vm, const char *id) {
    for (LzModule *module = vm->modules; module != NULL; module = module->next) {
        if (module->id != NULL && strcmp(module->id, id) == 0) {
            return module;
        }
    }
    return NULL;
}

static bool loading_push(LzVM *vm, const LzModule *module) {
    if (vm->loading_count == vm->loading_capacity) {
        size_t capacity = vm->loading_capacity == 0 ? 8 : vm->loading_capacity * 2;
        const LzModule **grown = alloc_fails() ? NULL : realloc((void *)vm->loading, capacity * sizeof *grown);
        if (grown == NULL) {
            return false;
        }
        vm->loading = grown;
        vm->loading_capacity = capacity;
    }
    vm->loading[vm->loading_count++] = module;
    return true;
}

static const char *display_name(const LzModule *module) {
    return module->source.name != NULL ? module->source.name : module->id;
}

static LzStatus circular_error(LzVM *vm, const LzModule *again) {
    char chain[240];
    size_t used = 0;
    size_t first = 0;
    while (first < vm->loading_count && vm->loading[first] != again) {
        first++;
    }
    chain[0] = '\0';
    for (size_t i = first; i < vm->loading_count && used < sizeof chain; i++) {
        used += (size_t)snprintf(chain + used, sizeof chain - used, "%s -> ", display_name(vm->loading[i]));
    }
    if (used < sizeof chain) {
        (void)snprintf(chain + used, sizeof chain - used, "%s", display_name(again));
    }
    return lz_vm_set_error(vm, LZ_ERR_RUNTIME, "circular import: %s", chain);
}

/* Runs the top-level code of a module once. Nothing is running: a plain run. Else a nested one. */
static LzStatus run_module_code(LzVM *vm, const LzChunk *chunk) {
    if (vm->frame_count == 0) {
        return lz_vm_run(vm, chunk);
    }
    if (vm->frame_count >= LZ_FRAMES_MAX || vm->stack_count + 1 > LZ_STACK_MAX) {
        return lz_vm_set_error(vm, LZ_ERR_STACK_OVERFLOW, "stack overflow: modules nested too deeply");
    }
    size_t depth = vm->frame_count;
    vm->stack[vm->stack_count++] = lz_value_nil(); /* the slot a callee takes, which the return drops */
    vm->frames[vm->frame_count++] =
        (LzCallFrame){NULL, chunk, 0, vm->stack_count, LZ_ALL_SUPPLIED, vm->native_call_offset};
    size_t failed_at = 0;
    LzStatus status = lz_vm_run_until(vm, depth, &failed_at);
    if (status == LZ_OK) {
        lz_value_release(&vm->heap, &vm->stack[--vm->stack_count]); /* the nil the return left */
        return LZ_OK;
    }
    if (vm->error_offset == LZ_NO_OFFSET && vm->frame_count > 0) {
        const LzChunk *failing = vm->frames[vm->frame_count - 1].chunk;
        if (failed_at < failing->count) {
            vm->error_offset = failing->offsets[failed_at];
            vm->error_source = lz_chunk_source(failing);
        }
    }
    if (!vm->trace_fixed) {
        lz_vm_capture_trace(vm);
        vm->trace_fixed = true;
    }
    size_t base = vm->frames[depth].base;
    vm->frame_count = depth;
    while (vm->handler_count > 0 && vm->handlers[vm->handler_count - 1].frame >= depth) {
        vm->handler_count--;
    }
    lz_vm_close_upvalues(vm, base);
    while (vm->stack_count > base - 1) {
        lz_value_release(&vm->heap, &vm->stack[--vm->stack_count]);
    }
    return status;
}

static void unlink_module(LzVM *vm, LzModule *module) {
    LzModule **link = &vm->modules;
    while (*link != NULL && *link != module) {
        link = &(*link)->next;
    }
    if (*link == module) {
        *link = module->next;
        vm->module_count--;
    }
}

LzStatus lz_module_load_source(LzVM *vm, LzModuleSource *source, LzModule **out) {
    LzModule *known = source->id != NULL ? find_module(vm, source->id) : NULL;
    if (known != NULL) {
        LzStatus status = LZ_OK;
        if (known->state == LZ_MODULE_LOADING) {
            status = circular_error(vm, known);
        } else if (out != NULL) {
            *out = known;
        }
        source_discard(source);
        return status;
    }
    if (source->id == NULL || source->name == NULL || source->text == NULL) {
        source_discard(source);
        return lz_vm_set_error(vm, LZ_ERR_RUNTIME, "the module source is incomplete");
    }
    if (vm->module_count >= LZ_MODULES_MAX) {
        source_discard(source);
        return lz_vm_set_error(vm, LZ_ERR_RUNTIME, "too many modules (limit is %d)", LZ_MODULES_MAX);
    }

    LzModule *module = mod_calloc(sizeof *module);
    LzModuleFailure *failure = module != NULL ? mod_calloc(sizeof *failure) : NULL;
    LzChunk *chunk = failure != NULL ? mod_calloc(sizeof *chunk) : NULL;
    if (chunk == NULL) {
        free(module);
        free(failure);
        source_discard(source);
        return lz_vm_out_of_memory(vm);
    }
    module->id = source->id;
    module->source.name = source->name;
    module->source.text = source->text;
    module->source.length = source->length;
    module->source.owned = true;
    source->id = NULL;
    source->name = NULL;
    source->text = NULL;
    lz_table_init(&module->globals);
    module->state = LZ_MODULE_LOADING;
    module->chunk = chunk;
    lz_chunk_init(chunk);

    if (!loading_push(vm, module)) {
        free(failure);
        lz_module_discard(module);
        return lz_vm_out_of_memory(vm);
    }
    module->next = vm->modules;
    /* Append, so main stays first. */
    LzModule **tail = &vm->modules;
    while (*tail != NULL) {
        tail = &(*tail)->next;
    }
    module->next = NULL;
    *tail = module;
    vm->module_count++;

    LzStatus status = lz_compile_module(vm, module, &module->source, chunk, &failure->diagnostics);
    if (status != LZ_OK) {
        if (status == LZ_ERR_COMPILE && failure->diagnostics.count > 0) {
            const LzDiagnostic *first = &failure->diagnostics.items[0];
            LzLocation at = lz_source_locate(&module->source, first->offset);
            (void)lz_vm_set_error(vm, LZ_ERR_COMPILE, "cannot load module '%s': %s (%s:%u:%u)", module->id,
                                  first->message, module->source.name, (unsigned)at.line, (unsigned)at.column);
        }
        /* The host reads the diagnostics against the module's text, so the record takes the source. */
        failure->source = module->source;
        module->source.name = NULL;
        module->source.text = NULL;
        module->source.owned = false;
        unlink_module(vm, module);
        vm->loading_count--;
        lz_chunk_free(&vm->heap, chunk);
        lz_table_free(&vm->heap, &module->globals);
        lz_module_discard(module);
        lz_module_failure_free(vm);
        vm->module_failure = failure;
        return status;
    }
    free(failure);

    status = run_module_code(vm, chunk);
    vm->loading_count--;
    if (status != LZ_OK) {
        /* What the code made may live on in other places, so the module's memory stays until the VM ends. */
        unlink_module(vm, module);
        lz_chunk_free(&vm->heap, chunk);
        lz_chunk_init(chunk);
        lz_table_free(&vm->heap, &module->globals);
        module->state = LZ_MODULE_READY;
        module->next = vm->retired;
        vm->retired = module;
        return status;
    }
    module->state = LZ_MODULE_READY;
    if (out != NULL) {
        *out = module;
    }
    return LZ_OK;
}

LzStatus lz_module_load(LzVM *vm, const LzModule *importer, const char *name, LzModule **out) {
    if (vm->resolver.resolve == NULL) {
        return lz_vm_set_error(vm, LZ_ERR_RUNTIME, "modules are not available here");
    }
    LzModuleSource source;
    memset(&source, 0, sizeof source);
    LzStatus status = vm->resolver.resolve(vm->resolver.context, vm, importer, name, &source);
    if (status != LZ_OK) {
        source_discard(&source);
        if (vm->error[0] == '\0') {
            (void)lz_vm_set_error(vm, status, "cannot find module '%s'", name);
        }
        return status;
    }
    return lz_module_load_source(vm, &source, out);
}

bool lz_module_set_main(LzVM *vm, const char *id, const char *name) {
    LzModule *main = vm->main_module;
    if (main == NULL) {
        return false;
    }
    char *id_copy = alloc_fails() ? NULL : malloc(strlen(id) + 1);
    char *name_copy = alloc_fails() ? NULL : malloc(strlen(name) + 1);
    if (id_copy == NULL || name_copy == NULL || !loading_push(vm, main)) {
        free(id_copy);
        free(name_copy);
        return false;
    }
    memcpy(id_copy, id, strlen(id) + 1);
    memcpy(name_copy, name, strlen(name) + 1);
    free(main->id);
    free((void *)main->source.name);
    main->id = id_copy;
    main->source.name = name_copy;
    main->state = LZ_MODULE_LOADING; /* main is running until the program ends */
    return true;
}
