#ifndef LIZARD_MODULE_H
#define LIZARD_MODULE_H

#include "lizard/chunk.h"
#include "lizard/common.h"
#include "lizard/source.h"
#include "lizard/table.h"

/* One import chain can't be longer than this, and a VM can't hold more modules than this. */
#define LZ_MODULES_MAX 1024

typedef enum { LZ_MODULE_LOADING, LZ_MODULE_READY } LzModuleState;

/*
 * A module is one source with a global table of its own. The program the host runs is the module
 * `main`, whose table is vm->globals. See docs/MODULES.md.
 */
typedef struct LzModule {
    char *id;               /* canonical id (e.g. absolute path), the cache key; owned */
    LzSource source;        /* owned text and name, for reports */
    LzTable globals;        /* this module's globals; for main, vm->globals is used instead */
    LzModuleState state;
    LzChunk *chunk;         /* its top-level code, kept for its interface; NULL for main */
    struct LzModule *next;  /* the VM's list of modules */
    bool is_main;
} LzModule;

typedef struct LzModuleSource { /* what a resolver returns */
    char *id;      /* canonical id; the resolver allocates, the VM frees */
    char *name;    /* shown in reports (file name); the resolver allocates, the VM frees */
    char *text;    /* the source; the VM frees */
    size_t length;
    /* reserved for later, always NULL/0 for now: */
    const char *origin;          /* which package or location it came from */
    const char *native_library;  /* path of a native library, if any */
    const void *permissions;     /* what the module may do */
} LzModuleSource;

typedef struct LzModuleResolver {
    void *context;
    /*
     * importer may be NULL (the host asks for a root module). Return LZ_OK and fill *out, or an
     * error status with vm->error set ("cannot find module 'x'").
     */
    LzStatus (*resolve)(void *context, LzVM *vm, const LzModule *importer, const char *name,
                        LzModuleSource *out);
    void (*destroy)(void *context); /* may be NULL */
} LzModuleResolver;

/* The globals that code of this chunk reads and writes: its module's table, or vm->globals. */
LzTable *lz_chunk_globals(LzVM *vm, const LzChunk *chunk);

/* A global of the table, then a builtin. name must be interned. NULL when undefined. */
const LzValue *lz_module_lookup(const LzVM *vm, const LzTable *globals, const LzString *name);

/* The source of the module the chunk belongs to, or NULL for main (whose source the host keeps). */
const LzSource *lz_chunk_source(const LzChunk *chunk);

/* The globals table of a module, where NULL means main. */
LzTable *lz_module_table(LzVM *vm, const LzModule *module);

/*
 * The module of the code that is running: the one of the chunk in the top frame. NULL means main,
 * the same as LzChunk.module. A native pushes no frame, so inside one this is the module of the
 * code that called it. With no frame at all it's main.
 */
const LzModule *lz_vm_scope(const LzVM *vm);

/* A global of the running module, then a builtin. NULL when undefined. */
const LzValue *lz_vm_scope_lookup(const LzVM *vm, const LzString *name);

/*
 * Finds, compiles and runs a module once. importer may be NULL (the host loads a root module).
 * A module that is already loaded is returned as it is and its code does not run again.
 */
LzStatus lz_module_load(LzVM *vm, const LzModule *importer, const char *name, LzModule **out);

/* Like lz_module_load, but the host gives the text. The VM takes the memory in source. */
LzStatus lz_module_load_source(LzVM *vm, LzModuleSource *source, LzModule **out);

/* A global of the module, or NULL. It doesn't look at builtins. */
const LzValue *lz_module_get(const LzModule *module, const LzString *name);

/*
 * Tells the VM what the main program is: its canonical id and the name of its file. An import that
 * leads back to it is then a cycle. The text is copied. False when out of memory.
 */
bool lz_module_set_main(LzVM *vm, const char *id, const char *name);

/* Installs the resolver and destroys the previous one. */
void lz_vm_set_resolver(LzVM *vm, const LzModuleResolver *resolver);

/* Name of the module's source for reports, or NULL for main. */
const char *lz_module_name(const LzModule *module);

/*
 * Test hook: the n-th allocation made by the module loader from now on fails (1 is the next one),
 * and 0 turns the hook off. For the memory-safety tests.
 */
void lz_module_fail_after(size_t n);

/* Internal, used by lz_vm_init, lz_vm_free and the loader. */
void lz_module_discard(LzModule *module); /* frees one module that is not in any list */
void lz_module_failure_free(LzVM *vm);    /* drops the failure record */
void lz_modules_init(LzVM *vm);
void lz_modules_release_tables(LzVM *vm); /* drops every module's globals, before the heap goes */
void lz_modules_free(LzVM *vm);           /* frees the modules, the resolver and the failure record */

#endif
