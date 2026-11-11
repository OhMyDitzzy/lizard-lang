# Modules

A module is one piece of source with a global table of its own. The program you run with
`lizard script.lz` is a module too, called `main`. This chapter describes what exists today: the
machinery that loads and separates modules. There is no `import` statement yet, no `std.`, no
packages, no permissions and no native code. A host program loads modules through the C API, and
the tests use that API. The syntax comes later and will sit on top of what is described here.

## Globals belong to a module

Every `let`, `const`, `fn` and `class` at the top level of a module defines a name in that
module's own table. Two modules can both have a global called `x`, and neither sees the other's.

```lizard
let x = 1
fn get_x() => x
println(get_x())    # 1
```

Code reads and writes the table of the module it was written in, whoever calls it. A function from
module `a` that reads `base` reads the `base` of `a`, even when `main` calls it, passes it to
`map` or keeps it as a closure.

The builtins (`println`, `len`, ...) belong to the VM and every module sees them. A global of a
module hides a builtin of the same name, as it does in a single file today.

Values are not tied to modules. A class made in `a` can have instances in `main`, and `typeof` and
`is` work on them. Only the *names* are per module. Two classes with the same name in different
modules are two different classes, and each one remembers where it came from
(`lz_class_module_name`), so later error messages can say which one they mean.

## `extend` stays in the module that writes it

An `extend` block is private to the module that contains it, and it is lexical: it is seen by the
code written in that module and by nothing else. There are no exceptions and nothing is
inherited from one module to another.

```lizard
extend string {
    pub fn shout(self) => self.to_upper() + "!"
}
println("hi".shout())    # HI!
```

If this lives in module `a`, then `"hi".shout()` in `main` is an error, and so is the same call
through a variable of type `any`, through a closure that `map` runs, or through a getter. The
reverse holds too. Two modules may add the same name to `string` without any clash, and an
`extend` of a class that another module wrote is visible only in the module that wrote the block.

The reason is that a type's behavior should not change because of a module far away. If any
module could add `shout` to `string` for everyone, then reading `s.shout()` would need a search
through every module that was ever loaded. This way the rule is local: look at your own file.

Because other modules never see an extension, `pub` makes no sense in front of it, and it is
refused:

```text
error: 'pub' cannot be used on 'extend'
 --> main.lz:1:1
  |
1 | pub extend string {
  | ^^^ an extension stays inside the module that writes it
  |
  = note: other modules never see it, so it cannot change how they use string
help: remove `pub`, or share the behavior with a `pub fn` that takes the value
  |
1 | extend string {
  | - remove `pub `
```

If you want other modules to use the behavior, share a function:

```lizard
fn shout(s: string) => s.to_upper() + "!"
println(shout("hi"))    # HI!
```

Code that runs *now* decides which blocks are visible: the module of the chunk in the top call
frame. A native function such as `map` pushes no frame, so a callback it runs is judged by the
module of the callback, and a native that looks up a method is judged by the module of the code
that called it.

## Loading a module

The host installs a resolver. The language knows nothing about files.

```c
typedef struct LzModuleResolver {
    void *context;
    LzStatus (*resolve)(void *context, LzVM *vm, const LzModule *importer, const char *name,
                        LzModuleSource *out);
    void (*destroy)(void *context); /* may be NULL */
} LzModuleResolver;
```

`resolve` gets the name that was asked for and the module that asked (`NULL` when the host itself
asks for a root module). It fills `out` with a canonical `id` (the cache key, for example an
absolute path), a `name` for reports, and the source `text`. The resolver allocates these and the
VM frees them. On failure it returns an error status and sets `vm->error`, for example
`cannot find module 'x'`.

`LzModuleSource` also has `origin`, `native_library` and `permissions`. They are always `NULL`
today. They are there so that packages, native libraries and permissions can arrive later without
changing the resolver's signature and breaking every host that wrote one.

`lz_vm_set_resolver(vm, &resolver)` installs it. A fresh VM has none, and then
`lz_module_load` fails with `modules are not available here`. `lz_module_load(vm, importer, name,
&module)` finds, compiles and runs a module. `lz_module_load_source` does the same when the host
already has the text. `lz_module_get(module, name)` reads a global of a module. For `main` the
globals are `vm->globals`, so use `lz_vm_lookup` there.

A module runs its top-level code **once**, when it is first loaded, and side effects are allowed.
Loading it again returns the same module and runs nothing. If it fails to compile or to run, it is
not cached: the next attempt starts again. The text of a failed compile is kept in
`lz_vm_module_failure(vm)` with all the diagnostics, for the host to show.

A module's code runs under the same limits as everything else: depth, fuel and `--max-heap` apply.

### Circular imports

While a module loads, it is marked as loading. If the resolver returns the id of a module that is
still loading, the load fails and names the chain:

```text
circular import: a.lz -> b.lz -> a.lz
```

The program that the command line runs counts as loading until it ends, so an import that comes
back to the script is found too.

### Writing a resolver

A resolver for modules kept in memory is a few lines. It looks the name up in a table of
`{name, text}` and returns copies:

```c
static LzStatus resolve(void *context, LzVM *vm, const LzModule *importer, const char *name,
                        LzModuleSource *out) {
    const Table *table = context;
    const char *text = table_find(table, name);
    if (text == NULL) {
        return lz_vm_set_error(vm, LZ_ERR_RUNTIME, "cannot find module '%s'", name);
    }
    out->id = copy(name);
    out->name = copy(name);
    out->text = copy(text);
    out->length = strlen(text);
    return LZ_OK;
}
```

`tests/test_modules.c` has a full version. The command line uses `lz_file_resolver_install`
(`include/lizard/module_file.h`), which you may use too. It turns `a.b.c` into the file `a/b/c.lz`
next to the file of the importer. Every part of a name must be an identifier, so `..`, slashes and
absolute paths cannot occur. The id is the real path of the file, so two names for one file give
one module. The `std` prefix answers `the standard library is not available yet`.

## Limits

* The kind of an error (`error X : ...`) is shared by the whole VM, not per module.
* A VM holds at most 1024 modules (`too many modules (limit is 1024)`).
* There is no `import` or `from` syntax, no `std.`, no packages, no permissions and no native
  modules.
* A module's top-level code always runs on the reference interpreter. Functions it defines run on
  whichever engine the program uses, and read the module's own globals on all of them.

## Errors that cross modules

A report names the file of each line. When code in `a.lz` fails under a call from `main.lz`, the
failing line is shown from `a.lz`, and the call stack lists `a.lz:3:5` and then `main.lz:7:1`. A
program that is one file reports exactly as before.
