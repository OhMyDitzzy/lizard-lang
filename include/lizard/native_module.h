#ifndef LIZARD_NATIVE_MODULE_H
#define LIZARD_NATIVE_MODULE_H

/*
 * The VM side of lzapi.h, see docs/NATIVE.md. Embedders only need lz_vm_register_native_module
 * (declared in lizard/module.h). The rest is for the VM itself.
 */

#include "lizard/class.h"
#include "lizard/native.h"
#include "lizard/module.h"
#include "lizard/vm.h"
#include "lzapi.h"
#include "lizard/lexer.h"

typedef struct LznLiteralReader {
    void (*init)(LzLexer *, const LzSource *);
    LzToken (*next)(LzLexer *);
    bool (*string)(const LzSource *, const LzToken *, LzBuffer *);
} LznLiteralReader;
extern LznLiteralReader lz_native_literals;

struct LznState;

/*
 * Checks the types written in one signature by reading them as Lizard does. param_types and
 * result may be NULL. `declared` are the names of the module's own classes and errors, which are
 * legal type names. Returns true when all parse, else writes the reason into err.
 */
typedef bool (*LznSigCheck)(LzVM *vm, const char *const *declared, size_t declared_count,
                            const char *const *types, size_t type_count, char *err, size_t err_size);

/* Runs init now and builds the module. The module joins the VM's list at its first import. */
LzStatus lz_native_module_build(LzVM *vm, const char *name, LznInitFn init, LznSigCheck check);
/* The module registered under this name, or NULL. Makes sure it is in the VM's module list. */
LzModule *lz_native_module_find(LzVM *vm, const char *name, bool *known);

/* Called from lz_vm_free. */
void lz_native_vm_shutdown(LzVM *vm); /* finalizers, then lasting references, before the heap goes */
void lz_native_vm_free(LzVM *vm);     /* after the modules are freed */

bool lz_native_module_arity(LzVM *vm, const LzNative *native, int argc, LzStatus *status);

/* Native classes: hooks for class.c. */
size_t lz_native_instance_extra(const LzClass *cls);
void lz_native_instance_setup(LzInstance *instance);
void lz_native_instance_finalize(LzInstance *instance);
LzStatus lz_native_class_construct(LzVM *vm, LzClass *cls, size_t positional, const LzValue *argv,
                                   size_t named_count, LzValue *result);
/* Does this class carry native data? */
bool lz_native_class_is(const LzClass *cls);
void *lz_native_instance_data(const LzInstance *instance);
LzInstance *lz_instance_new_raw(LzVM *vm, LzClass *cls);

#endif
