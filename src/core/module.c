#include "lizard/module.h"
#include "lizard/compiler.h"
#include "lizard/vm.h"

#include <stdlib.h>
#include <string.h>

const LzValue *lz_module_lookup(const LzVM *vm, const LzTable *globals, const LzString *name) {
    const LzValue *value = lz_table_get(globals, name);
    return value != NULL ? value : lz_table_get(&vm->builtins, name);
}

const LzSource *lz_chunk_source(const LzChunk *chunk) {
    const LzModule *module = chunk->module;
    return module == NULL || module->is_main ? NULL : &module->source;
}

LzTable *lz_module_table(LzVM *vm, const LzModule *module) {
    return module == NULL || module->is_main ? &vm->globals : (LzTable *)&module->globals;
}

const LzModule *lz_vm_scope(const LzVM *vm) {
    if (vm->frame_count == 0) {
        return NULL;
    }
    return vm->frames[vm->frame_count - 1].chunk->module;
}

const LzValue *lz_vm_scope_lookup(const LzVM *vm, const LzString *name) {
    const LzModule *module = lz_vm_scope(vm);
    const LzTable *table = module == NULL || module->is_main ? &vm->globals : &module->globals;
    return lz_module_lookup(vm, table, name);
}

const LzValue *lz_module_get(const LzModule *module, const LzString *name) {
    /* The globals of main are vm->globals, so ask lz_vm_lookup about those. */
    return lz_table_get(&module->globals, name);
}

const char *lz_module_name(const LzModule *module) {
    return module == NULL || module->is_main ? NULL : module->source.name;
}

void lz_modules_init(LzVM *vm) {
    vm->modules = NULL;
    vm->main_module = NULL;
    vm->module_count = 0;
    vm->retired = NULL;
    vm->resolver.context = NULL;
    vm->resolver.resolve = NULL;
    vm->resolver.destroy = NULL;
    vm->loading = NULL;
    vm->loading_count = 0;
    vm->loading_capacity = 0;
    vm->module_failure = NULL;
    vm->last_failure = NULL;
    vm->import_cycle = false;
    vm->compiling = 0;
}

void lz_module_discard(LzModule *module) {
    free(module->id);
    free((void *)module->source.name);
    lz_source_free(&module->source);
    free(module->chunk);
    free(module);
}

static void module_chunk_release(LzVM *vm, LzModule *module) {
    if (module->chunk != NULL) {
        lz_chunk_free(&vm->heap, module->chunk);
        lz_chunk_init(module->chunk);
    }
}

void lz_modules_release_tables(LzVM *vm) {
    for (LzModule *list = vm->modules; list != NULL; list = list->next) {
        module_chunk_release(vm, list);
        lz_table_free(&vm->heap, &list->globals);
    }
    for (LzModule *list = vm->retired; list != NULL; list = list->next) {
        lz_table_free(&vm->heap, &list->globals);
    }
}

void lz_module_failure_free(LzVM *vm) {
    while (vm->module_failure != NULL) {
        LzModuleFailure *failure = vm->module_failure;
        vm->module_failure = failure->next;
        free(failure->id);
        free((void *)failure->source.name);
        lz_source_free(&failure->source);
        free(failure);
    }
}

void lz_modules_free(LzVM *vm) {
    for (LzModule *list = vm->modules; list != NULL;) {
        LzModule *next = list->next;
        lz_module_discard(list);
        list = next;
    }
    for (LzModule *list = vm->retired; list != NULL;) {
        LzModule *next = list->next;
        lz_module_discard(list);
        list = next;
    }
    vm->modules = NULL;
    vm->retired = NULL;
    vm->main_module = NULL;
    vm->module_count = 0;
    free((void *)vm->loading);
    vm->loading = NULL;
    vm->loading_count = 0;
    vm->loading_capacity = 0;
    lz_module_failure_free(vm);
    lz_vm_set_resolver(vm, NULL);
}

void lz_vm_set_resolver(LzVM *vm, const LzModuleResolver *resolver) {
    if (vm->resolver.destroy != NULL) {
        vm->resolver.destroy(vm->resolver.context);
    }
    if (resolver == NULL) {
        vm->resolver.context = NULL;
        vm->resolver.resolve = NULL;
        vm->resolver.destroy = NULL;
    } else {
        vm->resolver = *resolver;
    }
}

const LzModuleFailure *lz_vm_module_failure(const LzVM *vm) {
    return vm->module_failure;
}

