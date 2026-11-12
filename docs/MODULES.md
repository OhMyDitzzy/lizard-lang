# Modules

A module is one piece of source with a global table of its own. The program you run with
`lizard script.lz` is a module too, called `main`. The first part of this chapter is for people who
write programs that use modules, and the last part, "For embedders", is for people who host the VM
from C. There is no `std.`, no packages, no permissions and no native code yet.

## Using modules in a program

A module is a `.lz` file. Names a module marks `pub` can be used by other files, and everything else
stays private to it.

```text
util.lz
pub fn add(a: int, b: int) -> int => a + b
pub const LIMIT = 10
fn hidden() => 1            # no pub, so only util.lz can call it
```

Another file brings the module in with `import`:

```text
import util
import util as u            # binds `u` instead of `util`
import tools.format         # the file tools/format.lz, bound as `format`
from util import add, LIMIT as MAX

println(util.add(1, 2))
println(add(3, 4) + MAX)
```

`a.b.c` means the file `a/b/c.lz`, looked up from the folder of the file that imports it. `import util`
binds a name that only exists at compile time: `util.add` is checked against what `util` exports, but
`util` alone is not a value, so `let m = util` is an error. `from util import add` binds `add` itself.

Imports come first in the file. Only comments and other imports may stand before them, and an import
inside a function or after other code is an error. That way every `util.f()` is already defined when
the code that uses it runs. A module's top-level code runs once, the first time an `import` of it is
reached, even when several files import it, and in the order the imports are written.

Only `pub` names can be imported. There is no `pub let`, because the importing file copies the value
once and a variable could change afterwards. There is no `pub extend` either: an `extend` stays in the
file that writes it. There is no `from util import *`, no re-exporting with `pub import`, and no import
that depends on a condition.

When something is wrong, the error shows up on the line of the import or of the name:

```text
error: cannot use 'secret' from module 'util'
 --> main.lz:2:14
  |
2 | println(util.secret())
  |              ^^^^^^ this name is private to util.lz
  |
  = note: `secret` exists in util but is not `pub`
```

A missing module and a module that does not compile are reported at the `import` line too. The
module's own report is printed first, with its own file name, and only once per run, however many
files import it.

## Classes, traits and errors across modules

A `pub class`, `pub trait`, `pub error` or `pub type` of another module is a type here. It works wherever a
local one does, and the name `util` is only a namespace:

```text
shapes.lz
pub trait Shape {
    fn area(self)
}
pub class Square : Shape {
    pub side: int
    pub fn area(self) => self.side * self.side
}
pub error ShapeError
```

```text
import shapes
from shapes import Square as Sq

let s: shapes.Square = shapes.Square(3)       # annotations: ?, [], dict and function types too
println(s is shapes.Square)                    # is, is not
class Rect : shapes.Shape {                    # a parent or a trait in a class head
    pub fn area(self) => 6
}
extend shapes.Square {                         # private to this file, like any extend
    pub fn doubled(self) => self.area() * 2
}
try {
    throw shapes.ShapeError("bad")
} catch shapes.ShapeError as e {
    println(e.message)
}
```

The compiler knows the class by the identity it reserved when `shapes.lz` compiled (it is in the
interface), so the type exists before `shapes.lz` has run. Members are checked as they are for a
class of the same file: the arguments of `shapes.Square(...)` against `init`, `pub read` fields,
private members, and methods when they are called. A wrong type says what it was given:

```lizard fail
import util
let p: util.Point = 3
```

```
error: type mismatch: expected Point, got 3
```

A type names a class by identity, not by name. Two different classes called `Point` are two types.
When both appear in one message they are written with their module, and otherwise the name stays
short:

```text
error: type mismatch: expected geo.Point, got pt.Point
 --> main.lz:4:20
  |
4 | let b: geo.Point = a
  |                    ^
```

`typeof` still gives the plain class name.

A `pub type` alias is read from its target, in the names of the module that wrote it, so
`pub type Points = Point[]` is `Point[]` here even when you never import `Point`.

### Signatures

The signature of a `pub fn` is checked in the importing file, and the types it names are read in
the context of the module that wrote it. You do not need to import `Point` to use a function that
takes or returns one:

```text
util.lz
pub class Point { pub x: int }
pub trait Shape { fn area(self) }
pub error Oops
pub type Pts = Point[]
pub fn make(n: int) -> Point { return Point(n) }
pub fn area(s: Shape) -> int { return s.area() }
pub fn first<T>(xs: T[]) -> T { return xs[0] }
pub fn apply<T, U>(x: T, f: fn(T) -> U) -> U { return f(x) }
```

```text
import util
let p = util.make(3)                 # p is a util.Point, and p.x is checked
let n: int = util.first([1, 2])      # T is worked out from the argument, as for a local fn
util.area(p)                         # error: argument 's' of util.area() expects Shape, got Point
```

Classes, traits, errors and aliases, arrays, dicts and optionals of them, function types and the
type parameters of a generic function all work this way. A name that is none of these makes that one
part `any`, and the call is still checked when it runs.

### Errors are per module

The kind of an error is named by its module and its name, like a class. Two modules can both write
`error NotFound`, and they are two kinds:

```text
a.lz       pub error NotFound
b.lz       pub error NotFound : IOError
```

```text
import a
import b
try {
    a.find("x")
} catch b.NotFound as e {      # does not catch the NotFound of a
    println("b")
} catch a.NotFound as e {
    println("a")
}
```

`catch`, `is`, `throw`, annotations (`x: b.NotFound`), `extend`, and a parent in a head
(`error E : util.Base`) all take the qualified name, and `from util import NotFound` binds the plain
one. Inside one module the old rules stand: declaring the same error twice, or an error and a
variable with one name, is a mistake. The name shown in a message, in `e.kind` and by `typeof` stays
the short `NotFound`. When two kinds with one name meet in a message, each is written with its module:

```text
error: type error: argument 'e' of only_a(): expected a.NotFound, got b.NotFound
```

The order of the imports does not matter. An error that carries fields (`pub error Base { pub code: int }`)
can be the parent of an error in another module and passes its fields on.

## Exports do not change

A `pub fn`, `pub class`, `pub trait`, `pub error` or `pub type` is copied by the files that import it,
so it cannot be assigned again in the module that declares it. A name without `pub` keeps the old
rules, and a `pub const` was never assignable.

```lizard fail
pub fn area() => 1
area = 2
```

```
error: cannot assign to 'area': it is pub, so other modules rely on it staying the same
```

## Cycles

Two modules cannot import each other, directly or through others. A cycle is one error, at the
`import` that closes it, with the way round in a note. The modules above it in the chain fail
without a second message:

```text
a.lz       import b
b.lz       import a
main.lz    import a
```

```text
error: circular import of 'a'
 --> b.lz:1:8
  |
1 | import a
  |        ^ this import leads back to a module that is waiting for it
  |
  = note: the chain is a.lz -> b.lz -> a.lz
  = help: a module cannot import another module that imports it, directly or not

error: aborting due to 1 previous error
```

The program you run counts as part of the chain, so an import that comes back to the script is a
cycle too.

## Names an import can take

`import x` and `import x as y` bind a namespace. If that name is a builtin function or type
(`string`, `print`, `len`, `int`), the import would hide it in the whole file, so it is an error and
offers another name:

```text
error: 'string' is a builtin, so this import needs another name
 --> main.lz:1:8
  |
1 | import string
  |        ^^^^^^ this name is taken by a builtin
  |
help: give the import another name
  |
1 | import string as strings
  |              +++++++++++
```

`from util import len` is allowed. It is a global of this module, and a global hides a builtin
here as it does in a single file.

## Errors that cross modules

A report names the file of each line. When code in `a.lz` fails under a call from `main.lz`, the
failing line is shown from `a.lz`, and the call stack lists `a.lz:3:5` and then `main.lz:7:1`. A
program that is one file reports exactly as before.

## Limits

* A module in a subfolder looks for its own imports relative to its own folder. There is no `..`
  and there is no project root yet, so `tools/format.lz` cannot `import util` from the folder above
  it. It fails with `cannot find module 'util'` and the path it tried. Packages will answer this later.
* A file can bind at most 256 imported names (`too many imported names in one file`). Import the
  module as a namespace instead of naming many of its members.
* A file can declare at most 256 classes and traits.
* A VM has at most 256 error kinds, built-in ones included, counted over all its modules. A kind of
  a module that failed to compile or run stays counted until the VM is freed.
* A chain of imports can be 128 files deep and a program can load 1024 modules
  (`too many modules (limit is 1024)`).
* There is no `std.`, no packages, no permissions and no native modules.
* A module's top-level code always runs on the reference interpreter. Functions it defines run on
  whichever engine the program uses, and read the module's own globals on all of them.

## For embedders

### Globals belong to a module

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

### `extend` stays in the module that writes it

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
  = help: to share the behavior, write a `pub fn` that takes the value, like `pub fn describe(value: string) => ...`
help: remove `pub`, the extension is for this module only
  |
1 | extend string {
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

### Loading a module

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

#### Circular imports

While a module loads, it is marked as loading. If the resolver returns the id of a module that is
still loading, the load fails and names the chain:

```text
circular import: a.lz -> b.lz -> a.lz
```

(The compiler turns this into the single error shown under Cycles, with the chain in a note.)

The program that the command line runs counts as loading until it ends, so an import that comes
back to the script is found too.

#### Compiling before running

`lz_module_prepare` finds and compiles a module, and the modules it imports, without running any code.
`lz_module_run` runs a prepared module once. `lz_module_load` does both. The compiler uses
`lz_module_prepare` for every `import`, which is how a cycle or a broken module is found before the
program starts.

#### Writing a resolver

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

### What to expect from a host

* The top-level code of a module always runs on the reference interpreter, whatever engine you chose.
  The functions it creates run on your engine.
* A module that fails to run is dropped from the list of modules, but its source is kept until
  `lz_vm_free`, because its functions and diagnostics may still be pointed to. A user who retries the
  same bad module again and again grows the memory of the VM until it is freed.
* A retry runs the top-level code of the module from the start, so a side effect it made before it
  failed can happen twice.
* Error kinds are per module. `lz_vm_error_kind_from_name(vm, module, ...)` looks a plain name up in
  `module` (`NULL` is main) and then among the built-in kinds.
