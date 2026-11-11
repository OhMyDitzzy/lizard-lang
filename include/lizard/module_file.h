#ifndef LIZARD_MODULE_FILE_H
#define LIZARD_MODULE_FILE_H

#include "lizard/module.h"

/*
 * A resolver that finds modules as files, for the command line and for hosts that want the same
 * rules. The VM never installs it by itself. A name like `a.b.c` is the file `a/b/c.lz`, looked up
 * next to the file of the importing module (for the main program, next to the script). Every part
 * of a name must be an identifier, so a name cannot hold `..`, a slash or an absolute path.
 */

/* Fills *out like any resolver. `context` is the LzFileResolver made by lz_file_resolver_new. */
LzStatus lz_file_resolve(void *context, LzVM *vm, const LzModule *importer, const char *name,
                         LzModuleSource *out);

/*
 * Makes the resolver for a main program in `script_path`, and tells the VM that main is that file,
 * so an import that comes back to it is seen as a cycle. Returns false when out of memory or when
 * the script path can't be resolved. The VM owns the result, see lz_vm_set_resolver.
 */
bool lz_file_resolver_install(LzVM *vm, const char *script_path);

#endif
