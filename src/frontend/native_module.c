#include "lizard/compiler.h"
#include "lizard/native_module.h"
#include "lizard/vm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Reads each type text of a signature the way Lizard reads a parameter type, by compiling a
 * throwaway declaration. The module's own classes and errors stand in as aliases of `any`.
 */
static bool check_types(LzVM *vm, const char *const *declared, size_t declared_count, const char *const *types,
                        size_t type_count, char *err, size_t err_size) {
    size_t length = 64;
    for (size_t i = 0; i < declared_count; i++) {
        length += strlen(declared[i]) + 16;
    }
    for (size_t i = 0; i < type_count; i++) {
        length += strlen(types[i]) + 32;
    }
    char *text = malloc(length);
    LzDiagnostics *diagnostics = calloc(1, sizeof *diagnostics);
    if (text == NULL || diagnostics == NULL) {
        free(text);
        free(diagnostics);
        (void)snprintf(err, err_size, "out of memory");
        return false;
    }
    size_t used = 0;
    for (size_t i = 0; i < declared_count; i++) {
        used += (size_t)snprintf(text + used, length - used, "type %s = any\n", declared[i]);
    }
    for (size_t i = 0; i < type_count; i++) {
        used += (size_t)snprintf(text + used, length - used, "fn __sig%zu(a: %s) {\n}\n", i, types[i]);
    }
    LzSource source;
    lz_source_from_string(&source, "<signature>", text);
    LzChunk chunk;
    lz_chunk_init(&chunk);
    LzStatus status = lz_compile(vm, &source, &chunk, diagnostics);
    bool ok = status == LZ_OK;
    if (!ok) {
        (void)snprintf(err, err_size, "%s",
                       diagnostics->count > 0 ? diagnostics->items[0].message : "the type does not parse");
    }
    lz_chunk_free(&vm->heap, &chunk);
    free(diagnostics);
    free(text);
    return ok;
}

LzStatus lz_vm_register_native_module(LzVM *vm, const char *name, LznInitFn init) {
    lz_native_literals = (LznLiteralReader){lz_lexer_init, lz_lexer_next, lz_token_string_value};
    return lz_native_module_build(vm, name, init, check_types);
}
