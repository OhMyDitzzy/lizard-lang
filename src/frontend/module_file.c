#define _POSIX_C_SOURCE 200809L /* realpath */

#include "lizard/module_file.h"

#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lizard/compiler.h"
#include "lizard/vm.h"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#ifdef _WIN32
#define LZ_SLASH '\\'
#else
#define LZ_SLASH '/'
#endif

typedef struct FileResolver {
    char *script_id;   /* canonical path of the main script */
    char *script_name; /* its file name, for messages */
} FileResolver;

static char *copy_string(const char *text) {
    size_t length = strlen(text);
    char *copy = malloc(length + 1);
    if (copy != NULL) {
        memcpy(copy, text, length + 1);
    }
    return copy;
}

static const char *base_name(const char *path) {
    const char *base = path;
    for (const char *p = path; *p != '\0'; p++) {
        if (*p == '/' || *p == '\\') {
            base = p + 1;
        }
    }
    return base;
}

static char *canonical_path(const char *path) {
#ifdef _WIN32
    char buffer[PATH_MAX];
    return _fullpath(buffer, path, sizeof buffer) != NULL ? copy_string(buffer) : NULL;
#else
    char buffer[PATH_MAX];
    return realpath(path, buffer) != NULL ? copy_string(buffer) : NULL;
#endif
}

static bool is_identifier_part(const char *start, size_t length) {
    if (length == 0 || !(isalpha((unsigned char)start[0]) || start[0] == '_')) {
        return false;
    }
    for (size_t i = 1; i < length; i++) {
        if (!(isalnum((unsigned char)start[i]) || start[i] == '_')) {
            return false;
        }
    }
    return true;
}

/* Every dot-separated part is an identifier, which rules out `..`, separators and absolute paths. */
static bool valid_name(const char *name) {
    const char *part = name;
    for (const char *p = name;; p++) {
        if (*p == '.' || *p == '\0') {
            if (!is_identifier_part(part, (size_t)(p - part))) {
                return false;
            }
            part = p + 1;
            if (*p == '\0') {
                return true;
            }
        }
    }
}

static bool is_std(const char *name) {
    return strcmp(name, "std") == 0 || strncmp(name, "std.", 4) == 0;
}

LzStatus lz_file_resolve(void *context, LzVM *vm, const LzModule *importer, const char *name,
                         LzModuleSource *out) {
    const FileResolver *resolver = context;
    if (!valid_name(name)) {
        return lz_vm_set_error(vm, LZ_ERR_RUNTIME,
                               "invalid module name '%s': use identifiers separated by dots", name);
    }
    if (is_std(name)) {
        return lz_vm_set_error(vm, LZ_ERR_RUNTIME, "the standard library is not available yet");
    }

    /* The folder of the importer: the one of the script for main, else the one of the module's file. */
    const char *importer_id = importer == NULL || importer->is_main ? resolver->script_id : importer->id;
    const char *importer_name = importer == NULL || importer->is_main ? resolver->script_name
                                                                     : base_name(importer->id);
    size_t folder_length = (size_t)(base_name(importer_id) - importer_id);
    size_t name_length = strlen(name);
    char *path = malloc(folder_length + name_length + 4);
    if (path == NULL) {
        return lz_vm_out_of_memory(vm);
    }
    memcpy(path, importer_id, folder_length);
    char *w = path + folder_length;
    for (size_t i = 0; i < name_length; i++) {
        *w++ = name[i] == '.' ? LZ_SLASH : name[i];
    }
    memcpy(w, ".lz", 4);

    char *id = canonical_path(path);
    if (id == NULL) {
        LzStatus status = lz_vm_set_error(vm, LZ_ERR_IO, "cannot find module '%s' (looked for %s, imported from %s)",
                                          name, path, importer_name);
        free(path);
        return status;
    }
    free(path);

    LzSource source;
    char error[200];
    if (!lz_source_read_file(&source, id, error, sizeof error)) {
        LzStatus status = lz_vm_set_error(vm, LZ_ERR_IO, "cannot load module '%s': %s", name, error);
        free(id);
        return status;
    }
    char *file_name = copy_string(base_name(id));
    if (file_name == NULL) {
        lz_source_free(&source);
        free(id);
        return lz_vm_out_of_memory(vm);
    }
    out->id = id;
    out->name = file_name;
    out->text = (char *)source.text; /* owned: the module takes it */
    out->length = source.length;
    out->origin = NULL;
    out->native_library = NULL;
    out->permissions = NULL;
    return LZ_OK;
}

static void resolver_destroy(void *context) {
    FileResolver *resolver = context;
    if (resolver != NULL) {
        free(resolver->script_id);
        free(resolver->script_name);
        free(resolver);
    }
}

bool lz_file_resolver_install(LzVM *vm, const char *script_path) {
    FileResolver *resolver = calloc(1, sizeof *resolver);
    if (resolver == NULL) {
        return false;
    }
    resolver->script_id = canonical_path(script_path);
    resolver->script_name = resolver->script_id != NULL ? copy_string(base_name(resolver->script_id)) : NULL;
    if (resolver->script_name == NULL || !lz_module_set_main(vm, resolver->script_id, resolver->script_name)) {
        resolver_destroy(resolver);
        return false;
    }
    LzModuleResolver installed = {resolver, lz_file_resolve, resolver_destroy};
    lz_vm_set_resolver(vm, &installed);
    return true;
}
