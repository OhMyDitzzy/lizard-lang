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

/* Is this module waiting on the way to the one that asks for it? */
static bool in_progress(const LzModule *module) {
    return module->state == LZ_MODULE_LOADING || module->state == LZ_MODULE_RUNNING;
}

LzStatus lz_module_prepare_source(LzVM *vm, LzModuleSource *source, LzModule **out) {
    vm->module_run = lz_module_run;
    LzModule *known = source->id != NULL ? find_module(vm, source->id) : NULL;
    if (known != NULL) {
        LzStatus status = LZ_OK;
        if (in_progress(known)) {
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
    vm->last_failure = NULL;
    for (const LzModuleFailure *seen = vm->module_failure; seen != NULL; seen = seen->next) {
        if (seen->id != NULL && source->id != NULL && strcmp(seen->id, source->id) == 0) {
            /* It failed already in this compile and said so, so it is not compiled and reported again. */
            vm->last_failure = seen;
            source_discard(source);
            vm->import_cycle = seen->cycle;
            return lz_vm_set_error(vm, LZ_ERR_COMPILE, "cannot load module: it failed to compile above");
        }
    }
    if (vm->module_count >= LZ_MODULES_MAX) {
        source_discard(source);
        return lz_vm_set_error(vm, LZ_ERR_RUNTIME, "too many modules (limit is %d)", LZ_MODULES_MAX);
    }
    if (vm->compiling >= LZ_IMPORT_DEPTH_MAX) {
        source_discard(source);
        return lz_vm_set_error(vm, LZ_ERR_RUNTIME, "imports are nested too deeply (limit is %d)", LZ_IMPORT_DEPTH_MAX);
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
    /* Append, so main stays first. */
    LzModule **tail = &vm->modules;
    while (*tail != NULL) {
        tail = &(*tail)->next;
    }
    module->next = NULL;
    *tail = module;
    vm->module_count++;

    bool outer_cycle = vm->import_cycle;
    vm->import_cycle = false;
    LzStatus status = lz_compile_module(vm, module, &module->source, chunk, &failure->diagnostics);
    vm->loading_count--;
    if (status != LZ_OK) {
        size_t id_length = strlen(module->id) + 1;
        failure->id = malloc(id_length);
        if (failure->id != NULL) {
            memcpy(failure->id, module->id, id_length);
        }
        bool only_cycle = vm->import_cycle;
        for (size_t i = 0; i < failure->diagnostics.count && only_cycle; i++) {
            only_cycle = strncmp(failure->diagnostics.items[i].message, "circular import", 15) == 0;
        }
        failure->cycle = only_cycle;
        vm->import_cycle = only_cycle;
        vm->last_failure = failure;
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
        lz_chunk_free(&vm->heap, chunk);
        lz_table_free(&vm->heap, &module->globals);
        lz_vm_error_scope_drop(vm, module); /* its kinds stay counted but no name finds them */
        lz_module_discard(module);
        failure->next = vm->module_failure; /* what this module imports, and that failed, stays behind it */
        vm->module_failure = failure;
        return status;
    }
    free(failure);
    vm->import_cycle = outer_cycle;
    module->state = LZ_MODULE_COMPILED;
    if (out != NULL) {
        *out = module;
    }
    return LZ_OK;
}

LzStatus lz_module_run(LzVM *vm, LzModule *module) {
    if (module->state == LZ_MODULE_READY) {
        return LZ_OK;
    }
    if (module->state != LZ_MODULE_COMPILED) {
        return circular_error(vm, module);
    }
    if (!loading_push(vm, module)) {
        return lz_vm_out_of_memory(vm);
    }
    module->state = LZ_MODULE_RUNNING;
    LzStatus status = run_module_code(vm, module->chunk);
    vm->loading_count--;
    module->state = LZ_MODULE_READY;
    if (status != LZ_OK) {
        /* What the code made may live on in other places, so the module's memory stays until the VM ends. */
        unlink_module(vm, module);
        lz_chunk_free(&vm->heap, module->chunk);
        lz_chunk_init(module->chunk);
        lz_table_free(&vm->heap, &module->globals);
        lz_vm_error_scope_drop(vm, module);
        module->next = vm->retired;
        vm->retired = module;
    }
    return status;
}

LzStatus lz_module_load_source(LzVM *vm, LzModuleSource *source, LzModule **out) {
    LzModule *module = NULL;
    LzStatus status = lz_module_prepare_source(vm, source, &module);
    if (status != LZ_OK) {
        return status;
    }
    status = lz_module_run(vm, module);
    if (status == LZ_OK && out != NULL) {
        *out = module;
    }
    return status;
}

static LzStatus resolve_source(LzVM *vm, const LzModule *importer, const char *name, LzModuleSource *source) {
    if (vm->resolver.resolve == NULL) {
        return lz_vm_set_error(vm, LZ_ERR_RUNTIME, "modules are not available here");
    }
    memset(source, 0, sizeof *source);
    LzStatus status = vm->resolver.resolve(vm->resolver.context, vm, importer, name, source);
    if (status != LZ_OK) {
        source_discard(source);
        if (vm->error[0] == '\0') {
            (void)lz_vm_set_error(vm, status, "cannot find module '%s'", name);
        }
    }
    return status;
}

LzStatus lz_module_prepare(LzVM *vm, const LzModule *importer, const char *name, LzModule **out) {
    LzModuleSource source;
    LzStatus status = resolve_source(vm, importer, name, &source);
    return status != LZ_OK ? status : lz_module_prepare_source(vm, &source, out);
}

LzStatus lz_module_load(LzVM *vm, const LzModule *importer, const char *name, LzModule **out) {
    LzModule *module = NULL;
    LzStatus status = lz_module_prepare(vm, importer, name, &module);
    if (status != LZ_OK) {
        return status;
    }
    status = lz_module_run(vm, module);
    if (status == LZ_OK && out != NULL) {
        *out = module;
    }
    return status;
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

static size_t failures_report(const LzModuleFailure *failure, bool color, const LzWriter *out) {
    if (failure == NULL) {
        return 0;
    }
    size_t before = failures_report(failure->next, color, out);
    lz_diagnostics_report_body(&failure->diagnostics, &failure->source, color, out);
    return before + failure->diagnostics.count + failure->diagnostics.dropped;
}

size_t lz_vm_module_failures_report(const LzVM *vm, bool color, const LzWriter *out) {
    return failures_report(vm->module_failure, color, out);
}
