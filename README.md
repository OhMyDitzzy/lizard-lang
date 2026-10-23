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

`int[]`, `string[]`, `int[][]`, `(int | string)[]` and `int?[]` are types. An
array stored under such a type is checked once and then restricted to it, so
`push(ys, "x")` fails on an `int[]`. An unannotated array holds anything.

More in `examples/`. Run one with `build/bin/lizard examples/fizzbuzz.lz`.

## Build

Requires a C11 compiler and CMake 3.16 or newer.

    cmake -S . -B build
    cmake --build build
    ctest --test-dir build --output-on-failure

The binary is written to `build/bin/lizard`.

### Build options

| Option | Default | Effect |
| --- | --- | --- |
| `CMAKE_BUILD_TYPE` | `Release` | `Debug`, `Release`, `RelWithDebInfo`, `MinSizeRel` |
| `LZ_BUILD_TESTS` | `ON` | Build the test suite |
| `LZ_WARNINGS_AS_ERRORS` | `OFF` | Compile with `-Werror` |
| `LZ_ENABLE_SANITIZERS` | `OFF` | AddressSanitizer and UBSan |
| `LZ_ENABLE_LTO` | `OFF` | Link-time optimization when supported |

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
| `--max-heap=N` | Limit live heap bytes (suffix `K`, `M` or `G`) |
| `--fuel=N` | Limit the number of executed instructions |
| `--engine=E` | Execution engine: `ge` (default, the fast one) or `ref` (the reference interpreter) |
| `--engine-stats` | Print what the engine did when the program ends |
| `--jit=M` | Machine code for hot code (x86-64, AArch64): `on` (default), `off` or `eager` |
| `--` | Stop option parsing |

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
