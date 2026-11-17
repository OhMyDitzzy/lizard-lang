# Native modules

A module written in C can be imported from Lizard like any other module, and it can call Lizard
functions back. This chapter is for the people who write such modules and for the embedders who
register them. The whole interface is one header, `include/lzapi.h`, which the Autotools and plain
builds install as `$(includedir)/lzapi.h`. It includes nothing but `<stdint.h>`, `<stddef.h>` and
`<stdbool.h>`, and it shows no layout of the VM. A test compiles a module against a directory that
holds only that header, so this stays true.

Native code is trusted. It runs with all the rights of the process, and nothing here is a sandbox.
The fuel, heap and depth limits of the VM still count, but only for what the module reports
through this API.

## How a module is made available

The embedder registers a module under a dotted name before the program is compiled:

```c
lz_vm_register_native_module(&vm, "test.nmod", nmod_init);
```

`import test.nmod` is then answered by that module, before the file resolver is asked. A name that
starts with `std.` is fine here, and only the file resolver refuses such names. The module has an
interface like a Lizard module has, so the importer checks argument and result types at compile
time, `from test.nmod import add, greet as hi` works, `catch n.NmodError` works, and a native
class is a type in annotations.

`init` runs once, inside `lz_vm_register_native_module`. It has to run that early because the
compiler needs the interface of the module before any code that imports it runs, and because a
signature that does not parse should fail when the module is registered and not when it is first
used. Registering a name twice is an error. A module whose `init` fails is not registered at all,
and `import` of it reports that it cannot be found.

## Values and handles

A value reaches C as a `LznValue *`, a handle that is good until the native function returns.
Arguments come from `lzn_arg(ctx, i)`, and `lzn_arg_count(ctx)` says how many there are. New values
come from `lzn_int`, `lzn_float`, `lzn_bool`, `lzn_nil`, `lzn_string`, `lzn_array` and `lzn_dict`.
`lzn_return(ctx, handle)` sets the result, and `lzn_return_int` and its relatives are shortcuts.

Handles live in a stack that belongs to the VM and is reused, so a call allocates nothing for them,
and every handle is dropped when the function returns. The module never counts references of
temporary values. A handle carries the number of the call that made it. A handle used after that
call, or in a nested call, is refused with `stale or invalid native handle`, and a build with
`--poison` aborts instead. There are at most 65536 live handles at once. A loop over a very large
array should read what it needs and not hold every element.

`lzn_to_int`, `lzn_to_float` and `lzn_to_bool` return `LZN_FAIL` when the value is of another kind,
and they never convert: an int is not a float for them. `lzn_string_data` gives a borrowed,
NUL-terminated pointer that is valid until the function returns.

## References that last

A callback that a timer or an event loop will call later has to outlive the call that handed it
over. `lzn_ref` turns a handle into a `LznRef *`, which holds the value strongly. `lzn_deref` gives
a handle to it in the current call and `lzn_unref` lets go. The VM releases the references that are
still alive when it is freed, after the finalizers of the native instances ran.

The cycle collector does not see references held by native code. A value that holds a function
that keeps the value alive through a `LznRef` is a cycle that nothing will break, so the module
lets go of such a reference itself. The `lzn_ref*` functions must not be called from another
thread.

## Errors

`return lzn_throw(ctx, kind, "format", ...)` sets the error and returns `LZN_FAIL`. The kind is a
built-in one (`LZN_ERR_TYPE`, `LZN_ERR_RUNTIME`, ...) or one the module declared in `init` with
`lzn_module_error(b, "NmodError", parent)`. A declared kind belongs to its module, so two modules
can both declare `NmodError` and `catch a.NmodError` does not catch the error of `b`. A native
function that fails without setting an error still reports a failure, as
`name(): failed without an error`.

`lzn_check_int(ctx, i, &out)` and its relatives check an argument and write
`argument 'b' of test.nmod.add() expects int, got "x"`. The VM checks the number
of arguments against the signature and fills omitted defaults before entering native code.

## Declaring a module

`init` gets a `LznBuilder` and declares everything through it: `lzn_module_fn`,
`lzn_module_const_int`, `_float`, `_string`, `_bool`, `lzn_module_error` and `lzn_module_class`. The
signature is the only source of argument names, types and arity. Use named parameters,
for example `"fn(a: int, b: int = 0) -> int"`. Defaults must be literal nil, booleans,
numbers or strings. Required parameters precede default parameters. A final `*rest: any`
accepts surplus arguments. Rest parameters cannot have defaults. Duplicate parameter
names fail registration. Legacy unnamed types remain accepted with names `arg1`, `arg2`.

The VM fills omitted defaults before entering native code. `lzn_arg_count(ctx)` always
includes every fixed parameter, followed by each supplied rest argument. Native code
receives rest arguments individually. Methods omit `self` from their signatures.
Constructors use the same syntax without a result type.

Native functions and constructors currently reject named arguments on the existing call
path. This API does not change that restriction.

The test module declares like this:

```c
LznStatus nmod_init(LznBuilder *b);
LznStatus nmod_init(LznBuilder *b) {
    g_inits++;
    g_err_nmod = lzn_module_error(b, "NmodError", LZN_ERR_RUNTIME);
    g_err_child = lzn_module_error(b, "NmodChild", g_err_nmod);
    if (g_err_nmod < 0 || g_err_child < 0) {
        return LZN_FAIL;
    }
    LznClassDef counter = {
        .size = sizeof(CounterData),
        .init = counter_init,
        .init_signature = "fn(value: int = 0, step: int = 1)",
        .finalize = counter_finalize,
        .methods = counter_methods,
        .fields = counter_fields,
    };
    if (lzn_module_class(b, "Counter", &counter) != LZN_OK) {
        return LZN_FAIL;
    }
    if (lzn_module_const_int(b, "VERSION", 3) != LZN_OK || lzn_module_const_float(b, "HALF", 0.5) != LZN_OK ||
        lzn_module_const_string(b, "NAME", "nmod") != LZN_OK || lzn_module_const_bool(b, "ON", true) != LZN_OK) {
        return LZN_FAIL;
    }
    struct {
        const char *name;
        LznFn fn;
        const char *sig;
    } fns[] = {
        {"add", nm_add, "fn(a: int, b: int) -> int"},
        {"scale", nm_scale, "fn(x: float, k: int) -> float"},
        {"greet", nm_greet, "fn(name: string) -> string"},
        {"flip", nm_flip, "fn(value: bool) -> bool"},
        {"kind", nm_kind, "fn(value: any) -> string"},
        {"nothing", nm_nothing, "fn()"},
        {"rest_args", nm_count_args, "fn(*rest: any) -> int"},
        {"mixed_args", nm_count_args, "fn(a: int = 7, *rest: any) -> int"},
        {"count_args", nm_count_args, "fn(a: any = nil, b: any = nil, c: any = nil) -> int"},
        {"strict", nm_strict, "fn(value: any) -> int"},
        {"sum", nm_sum, "fn(values: int[]) -> int"},
        {"range", nm_range, "fn(n: int) -> int[]"},
        {"poke", nm_poke, "fn(values: int[], index: int, value: int)"},
        {"make_dict", nm_make_dict, "fn(key: string, value: int) -> any"},
        {"dict_info", nm_dict_info, "fn(value: any) -> int"},
        {"fail", nm_fail, "fn(value: int)"},
        {"apply", nm_apply, "fn(f: fn(int) -> int, value: int) -> int"},
        {"store", nm_store, "fn(f: fn(int) -> int)"},
        {"fire", nm_fire, "fn(value: int) -> int"},
        {"clear", nm_clear, "fn()"},
        {"keep", nm_keep, "fn(value: any)"},
        {"remember_handle", nm_remember_handle, "fn()"},
        {"use_handle", nm_use_handle, "fn() -> int"},
        {"spin", nm_spin, "fn(value: int) -> int"},
        {"alloc", nm_alloc, "fn(value: int) -> int"},
        {"finalized", nm_finalized, "fn() -> int"},
        {"created", nm_created, "fn() -> int"},
    };
    for (size_t i = 0; i < sizeof fns / sizeof fns[0]; i++) {
        if (lzn_module_fn(b, fns[i].name, fns[i].fn, fns[i].sig) != LZN_OK) {
            return LZN_FAIL;
        }
    }
    return LZN_OK;
}
```

## Native classes

`lzn_module_class` takes a `LznClassDef` with the size of the opaque data, a constructor, a
finalizer, methods and read-only fields. The VM allocates the data together with the instance, and
`lzn_self_data(ctx)` returns it in the constructor, in methods and in field getters. The data starts
zeroed. `finalize` runs exactly once for each instance: when it is freed, or when the VM is freed
while the instance is still alive. It also runs for an instance whose constructor failed, so it has
to cope with data that is only partly set up. Buffers made with `lzn_alloc` in the constructor
are freed in `finalize` with `lzn_free(NULL, ...)`, which accepts a null context for exactly this.

An instance behaves like one of a Lizard class: `is`, `typeof`, type annotations and passing it
around all work, and the data is not visible from Lizard. A read-only field is a getter. These
are not supported: a Lizard class that extends a native class (`cannot extend a native class`),
and `copy()` or `with()` of an instance (`cannot copy an instance of the native class`).

The class of the test module:

```c
typedef struct CounterData {
    int64_t value;
    char *buffer;
    int64_t step;
} CounterData;

static LznStatus counter_init(LznCtx *ctx) {
    CounterData *d = lzn_self_data(ctx);
    g_created++;
    d->value = 0;
    d->step = 1;
    d->buffer = lzn_alloc(ctx, 64);
    if (d->buffer == NULL) {
        return LZN_FAIL;
    }
    if (lzn_arg_count(ctx) > 0 && lzn_check_int(ctx, 0, &d->value) != LZN_OK) {
        return LZN_FAIL;
    }
    if (lzn_arg_count(ctx) > 1 && lzn_check_int(ctx, 1, &d->step) != LZN_OK) {
        return LZN_FAIL;
    }
    if (d->value < 0) {
        return lzn_throw(ctx, g_err_nmod, "Counter starts at a negative value");
    }
    return LZN_OK;
}

static void counter_finalize(void *data) {
    CounterData *d = data;
    g_finalized++;
    lzn_free(NULL, d->buffer, 64);
    d->buffer = NULL;
}

static LznStatus counter_add(LznCtx *ctx) {
    CounterData *d = lzn_self_data(ctx);
    int64_t n = d->step;
    if (lzn_kind(ctx, lzn_arg(ctx, 0)) != LZN_NIL && lzn_check_int(ctx, 0, &n) != LZN_OK) {
        return LZN_FAIL;
    }
    d->value += n;
    return lzn_return_int(ctx, d->value);
}

static const LznMethodDef counter_methods[] = {
    {"add", counter_add, "fn(n: int? = nil) -> int"},
    {"merge", counter_merge, "fn(other: Counter) -> Counter"},
    {"apply_to", counter_apply_to, "fn(f: fn(int) -> int) -> int"},
    {NULL, NULL, NULL},
};

static const LznFieldDef counter_fields[] = {
    {"value", "int", counter_value},
    {NULL, NULL, NULL},
};
```

## Calling Lizard

`lzn_call(ctx, fn, nargs, args, &result)` runs a function or closure. When it fails, the error is
already set and the native function returns `LZN_FAIL` to pass it on. Fuel, `--max-heap` and the
depth of calls count as they do for any call. A native function that calls back cannot be
suspended. That is a deliberate limit of this version, until there are coroutines. `lzn_call` has
no context in a finalizer, so it cannot be used there.

```c
static LznStatus nm_apply(LznCtx *ctx) {
    LznValue *fn;
    int64_t x;
    if (lzn_check_function(ctx, 0, &fn) != LZN_OK || lzn_check_int(ctx, 1, &x) != LZN_OK) {
        return LZN_FAIL;
    }
    LznValue *args[1] = {lzn_int(ctx, x)};
    LznValue *result = NULL;
    if (lzn_call(ctx, fn, 1, args, &result) != LZN_OK) {
        return LZN_FAIL;
    }
    return lzn_return(ctx, result);
}
```

## Memory and fuel

`lzn_alloc`, `lzn_realloc` and `lzn_free` count against `--max-heap` and fail with the same
heap-limit error. `lzn_spend(ctx, units)` takes fuel for work that goes on for a long time inside
one native function, and fails with `instruction limit exceeded` when there is not enough. Every
native call already costs the base fuel of a call.

```c
static LznStatus nm_alloc(LznCtx *ctx) {
    int64_t size;
    if (lzn_check_int(ctx, 0, &size) != LZN_OK) {
        return LZN_FAIL;
    }
    char *block = lzn_alloc(ctx, (size_t)size);
    if (block == NULL) {
        return LZN_FAIL;
    }
    memset(block, 1, (size_t)size);
    lzn_free(ctx, block, (size_t)size);
    return lzn_return_int(ctx, size);
}
```

## Threads

The VM runs on one thread and every `lzn_*` function must be called from that thread. An interface
to queue work from other threads exists only as a plan.

## API at a glance

| Area | Functions |
| --- | --- |
| Arguments | `lzn_arg_count`, `lzn_arg` |
| Making values | `lzn_nil`, `lzn_bool`, `lzn_int`, `lzn_float`, `lzn_string`, `lzn_array`, `lzn_dict` |
| Results | `lzn_return`, `lzn_return_nil`, `_bool`, `_int`, `_float`, `_string` |
| Reading | `lzn_kind`, `lzn_kind_name`, `lzn_to_int`, `lzn_to_float`, `lzn_to_bool`, `lzn_string_data` |
| Arrays | `lzn_array_len`, `lzn_array_get`, `lzn_array_push`, `lzn_array_set` |
| Dicts | `lzn_dict_len`, `lzn_dict_get`, `lzn_dict_get_int`, `lzn_dict_has`, `lzn_dict_has_int`, `lzn_dict_set`, `lzn_dict_set_int` |
| References | `lzn_ref`, `lzn_deref`, `lzn_unref` |
| Errors | `lzn_throw`, `lzn_check_int`, `_float`, `_bool`, `_string`, `_array`, `_dict`, `_function` |
| Calling back | `lzn_call` |
| Memory and fuel | `lzn_alloc`, `lzn_realloc`, `lzn_free`, `lzn_spend` |
| Declaring | `lzn_module_fn`, `lzn_module_const_*`, `lzn_module_error`, `lzn_module_class`, `lzn_module_name`, `lzn_builder_fail` |
| Instances | `lzn_self_data` |

## Ownership and lifetime

* A handle is valid until the native function that got it returns. Do not store one.
* A string from `lzn_string_data` is valid for the same time. Copy it to keep it.
* A `LznRef` lives until `lzn_unref` or the end of the VM.
* A pointer from `lzn_alloc` belongs to the module and counts against the heap until `lzn_free`.
* The strings and arrays passed to the builder (names, signatures, the `LznClassDef` and the
  method tables) are only read while `init` runs. The module does not need to keep them.

## Limits of this version

* No `dlopen`, no manifest and no package: a module is registered by the embedder, statically.
* No permissions. A native module can do anything the process can.
* No synchronisation between threads, and no way to post work to the VM from another thread.
* `lzn_call` cannot be suspended.
* A Lizard class cannot extend a native class, and an instance cannot be copied.
* References held by native code are invisible to the cycle collector.
* Frames of native functions are not listed in stack traces. The messages name the function, as in
  `test.nmod.add()`.

## ABI policy

`LZN_ABI_VERSION` is 1. The header has no struct that a module reads, apart from the definition
structs (`LznClassDef`, `LznMethodDef`, `LznFieldDef`) that a module fills in, and those may only
grow at the end in a later version together with a new ABI version. Everything else is opaque
pointers and plain integers and enums. Functions are not removed or changed within one ABI
version.

## Planned

These are not built and nothing above depends on them:

* Loading a native module from a shared library with `dlopen`, described by a manifest.
* Permissions for each module.
* An interface for an event loop and a `lzn_post` to hand work to the VM from other threads.
* A `trace` callback so that the cycle collector can see references held by native code.
* How native calls that cannot be suspended fit with coroutines.
* A generator for stubs and documentation from the signatures.
