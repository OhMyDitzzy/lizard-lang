# Lizard

An interpreted programming language written in C. Source files use the `.lz`
extension.

Status: early. Scripts run: variables, constants, arithmetic, strings, `if`,
`while`, blocks, optional type annotations and the builtins `print`,
`println`, `int`, `float`, `string` and `bool`, and arrays (`[1, 2, 3]`,
indexing, slices, typed `int[]`, and `len`, `push`, `sort`, ... see below), and
functions (`fn`: typed parameters and results, defaults, named arguments,
variadics, closures, recursion; see below).

```
const GREETING = "Hello" + ", " + "Lizard"
type Mode = "r" | "w"

let mode: Mode = "r"
let name: string? = nil
let i: int = 1
while i <= 3 {
  println(GREETING, i, mode)
  i = i + 1
}
```

```
let xs = [1, 2, 3]
let ys: int[] = [4, 5]
xs[-1]            # 3
xs[0] = 10
xs[1:3]           # [2, 3]
xs + ys           # [10, 2, 3, 4, 5]
```

### Functions

```
fn area(w: float, h: float = 1.0) -> float { return w * h }   # types are optional
fn sum(*xs) { ... }                  # rest parameter: xs is an array
let twice = fn(f, x) { return f(f(x)) }   # anonymous functions are values

area(3.0, h = 2.0)                   # named arguments, in any order
sum(1, 2, *[3, 4])                   # * spreads an array into arguments
```

* A function whose body is one expression can be written `fn(x) => expression`, which is
  `fn(x) { return expression }`: `fn(n: int) -> bool => n > 2`. The body may start on the next line.
* Parameters and the result may carry types; a typed call that is wrong is a
  compile-time error when the callee is known, and a run-time error otherwise.
  A function with `-> T` must return on every path ("missing return").
* A default may use earlier parameters (`fn k(a, b = a * 2)`).
* Top-level functions can call each other regardless of order; a function nested
  in another captures the variables it uses (closures keep them alive).
* Calls nest up to 2048 deep. An error inside a function prints a stack trace.

### Arrays

Arrays are mutable and passed by reference. Negative indexes count from the
end, slices (`xs[a:b]`, either bound optional) and `+` build new arrays, and
`==` compares by content. Only `nil` and `false` are falsy, so `[]` is truthy.
Indexing out of range is a runtime error. A slice clamps instead.

| Builtin | Effect |
| --- | --- |
| `len(x)` | items of an array, or characters (code points) of a string |
| `push(xs, v)`, `pop(xs)` | append, or remove and return the last item |
| `insert(xs, i, v)`, `remove(xs, i)` | insert before index `i` (`i == len` appends), or remove and return item `i` |
| `clear(xs)`, `copy(xs)` | empty in place, shallow copy |
| `contains(xs, v)`, `index_of(xs, v)` | search by `==`; `index_of` gives `-1` when absent |
| `reverse(xs)`, `sort(xs)` | in place; `sort` needs all numbers or all strings, and is stable |
| `join(xs, sep)` | items to text, separated by `sep` |
| `range(n)`, `range(a, b)` | the ints `0..n-1` or `a..b-1` |
| `sum(xs)`, `min(xs)`, `max(xs)` | numbers only; `min` and `max` fail on an empty array |
| `map(xs, f)`, `filter(xs, f)` | a new array of `f(item)`, or of the items for which `f(item)` is truthy |
| `reduce(xs, f, init)` | folds the items: `acc = f(acc, item)`, starting from `init` |
| `each(xs, f)` | calls `f(item)` for every item, returns `nil` |
| `find(xs, f)`, `any(xs, f)`, `all(xs, f)` | first item with a truthy `f(item)` (or `nil`), or whether some / every item has one |

`int[]`, `string[]`, `int[][]`, `(int | string)[]` and `int?[]` are types. An
array stored under such a type is checked once and then restricted to it, so
`push(ys, "x")` fails on an `int[]`. An unannotated array holds anything.

More in `examples/`. Run one with `build/bin/lizard examples/fizzbuzz.lz`.

### Strings

One literal form covers plain, raw, multi-line and template strings: `[$][r]` + quotes.

```
let name = "Ana"
println($"Hello {name}! {1 + 2} {{braces}}")      # template
println($"{3.14159:.2f} {255:#06x} {name:>8}")    # format specs
println($"{name=}")                               # debug form: name="Ana"
println(r"C:\path\no\escapes")                    # raw
println($"""
    multi-line, indentation follows the closing quotes
    """)
```

See [docs/STRINGS.md](docs/STRINGS.md) for the full rules and `examples/strings.lz`.

## Build

Requires a C11 compiler and CMake 3.16 or newer.

    cmake -S . -B build
    cmake --build build
    ctest --test-dir build --output-on-failure

The binary is written to `build/bin/lizard`. `./build.sh` does the same and more: it detects the
platform, builds, runs the tests, and writes (optionally signed) release tarballs per platform and
ABI. See [docs/BUILDING.md](docs/BUILDING.md); `lizard --version` shows the commit and kind of build.

    ./build.sh check                      # build and test
    ./build.sh package --targets=all      # one tarball per target that has a toolchain
    ./build.sh package --official         # signed release (clean tree, GPG key)

### Build options

| Option | Default | Effect |
| --- | --- | --- |
| `CMAKE_BUILD_TYPE` | `Release` | `Debug`, `Release`, `RelWithDebInfo`, `MinSizeRel` |
| `LZ_BUILD_TESTS` | `ON` | Build the test suite |
| `LZ_WARNINGS_AS_ERRORS` | `OFF` | Compile with `-Werror` |
| `LZ_ENABLE_SANITIZERS` | `OFF` | AddressSanitizer and UBSan |
| `LZ_ENABLE_LTO` | `OFF` | Link-time optimization when supported |
| `LZ_ENABLE_JIT` | `ON` | The Gecko JIT (x86-64 and AArch64 on Linux and macOS; ignored elsewhere) |
| `LZ_STATIC` | `OFF` | Static executable |
| `LZ_BUILD_CHANNEL` | `local` | `local` (unsigned) or `official` (needs a clean tree and `LZ_SIGNING_KEY`) |

Example: `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DLZ_ENABLE_SANITIZERS=ON`

## Usage

    lizard [options] <script.lz> [script arguments]

| Option | Effect |
| --- | --- |
| `-h`, `--help` | Show help |
| `-V`, `--version` | Show the version |
| `--heap-stats` | Print heap statistics when the program ends |
| `--trace-memory` | Log every heap allocation and release to stderr |
| `--dump-tokens` | Print the tokens of the script and exit |
| `--max-heap=N` | Limit live heap bytes (suffix `K`, `M` or `G`); dead closure cycles are collected before an allocation is refused |
| `--fuel=N` | Limit the number of executed instructions |
| `--engine=E` | Execution engine: `ge` (default, the fast one) or `ref` (the reference interpreter) |
| `--engine-stats` | Print what the engine did when the program ends |
| `--jit=M` | Machine code for hot code (x86-64, AArch64): `on` (default), `off` or `eager` |
| `--skink=M` | Cycle collector for closures: `on` (default) or `off` |
| `--color=W` | Colour in error messages: `auto` (default: only on a terminal, `NO_COLOR` turns it off), `always` or `never` |
| `--` | Stop option parsing |

### Methods and callbacks

`receiver.name(args)` is `name(receiver, args)`: any function whose first parameter is the
receiver can be written as a method, builtins and your own functions alike.

```
let nums = [1, 2, 3, 4, 5, 6]
nums.filter(fn(n) => n > 2).map(fn(n) => n * 10)    # [30, 40, 50, 60]
nums.reduce(fn(acc, n) => acc + n, 0)               # 21
nums.map(string)                                    # any function value is a callback
fn double(n) { return n * 2 }
5.double()                                          # 10

nums                                                # a chain may continue on the next line
  .filter(fn(n) => n > 2)
  .len()
```

### Methods of values

Strings, numbers and arrays have methods of their own. They are called like the ones above
(`value.name(args)`), and the names are snake_case.

| For | Methods |
| --- | --- |
| every value | `to_string()`, `to_bool()` |
| int, float, string | `to_int()`, `to_float()` (a string is parsed) |
| string | `to_upper()`, `to_lower()`, `trim()`, `trim_start()`, `trim_end()`, `split(sep)`, `contains(s)`, `starts_with(s)`, `ends_with(s)`, `replace(old, new)`, `repeat(n)`, `index_of(s)`, `chars()`, `slice(from, to)`, `is_empty()` |
| array | `first()`, `last()` (`nil` when empty), `is_empty()`, and the functions above (`xs.len()`, `xs.contains(v)`, `xs.join(sep)`, ...) |
| int, float | `abs()`, `floor()`, `ceil()`, `round()`, `sqrt()`, `pow(n)`, `min(other)`, `max(other)` |

```
"  Hello, Lizard ".trim().to_lower()      # "hello, lizard"
"a,b,c".split(",")                        # ["a", "b", "c"]
"héllo".slice(1, 3)                       # "él"   (positions count characters, like len)
2.5.round(), 7.pow(2), 16.sqrt()          # 3, 49, 4.0
nums.map(fn(n) => n.to_string()).join("-")
```

Text is UTF-8: lengths and positions count characters and searching works on bytes.
`to_upper`/`to_lower` use Unicode's simple case mapping, one character to one (`é` to `É`,
`ж` to `Ж`, `α` to `Α`), so `ß` stays `ß` and there is no locale-specific rule (Turkish
dotless i, final sigma). `trim` removes Unicode white space, including the no-break space.
Bytes that are not valid UTF-8 pass through unchanged. `round` rounds halves away from zero,
and `floor`, `ceil` and `round` give an int. `slice` takes negative positions and clamps, like
an array slice. A kind that has no method of that name uses the function of the same name when
there is one, and otherwise fails with `int has no method 'to_lower' (it works on string)`.
A function or variable of your own with the same name always wins over a method.

A callback gets the item only. The array is visited up to the length it had when the call
began, so a callback that pushes to it cannot loop forever; items it removes are not visited.
Callbacks run on the reference interpreter and may nest 128 deep. An error inside a callback
points at the failing line there, and the stack trace goes through to the call that started it.
Fields (`value.name` without a call) do not exist yet.

## Error messages

Errors show the source line, the offending span, and, when a name does not exist,
a "did you mean" suggestion drawn as the corrected line (`+` letters to add,
`~` letters to change, `-` letters to remove):

```
error: undefined function 'printj'
 --> hello.lz:2:1
  |
2 | printj("Hello World!")
  | ^^^^^^ not found in this scope
  |
help: did you mean `print`?
  |
2 | print("Hello World!")
  |      - remove `j`
  |
help: or maybe `println`?
  |
2 | println("Hello World!")
  |      ~~
```

Candidates are the builtins, the functions and variables of the script that are in
scope at that point, type names (`let x: intt`) and parameter names (`f(hieght = 1)`).
Names that later come from `import` need no extra work: once they are symbols or
globals of the VM they are offered too (see `suggest_names` in `src/frontend/compiler.c`
and `include/lizard/suggest.h`). A call is only matched with things that can be called.

## Engines

Programs run on the Gecko Engine (`ge`) by default: a faster engine for the same
bytecode. It is an optimized interpreter (about 3 to 4 times quicker than `ref`)
plus, on x86-64 and AArch64 (Linux, Android/Termux, macOS), a JIT that compiles hot loops and functions to machine
code (about 8 to 30 times quicker than `ref` on x86-64). `--jit=off` turns the JIT off.
`--engine=ref` selects the plain reference interpreter, which defines the
language and behaves identically. `cmake --build build --target bench` compares
the two on `bench/*.lz`. See [docs/ENGINE.md](docs/ENGINE.md).

## Development

`.clangd` reads `build/compile_commands.json`, so configure once before
opening the project in an editor. `.clang-tidy` and `.clang-format` are at the
repository root.

See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for the layout, the memory
model, and how to add a builtin.
