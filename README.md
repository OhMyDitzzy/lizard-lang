# Lizard

An interpreted programming language written in C. Source files use the `.lz`
extension.

Status: early. Scripts run: variables, constants, arithmetic, strings, `if`,
`while`, blocks, optional type annotations and the builtins `print`,
`println`, `int`, `float`, `string` and `bool`, and arrays (`[1, 2, 3]`,
indexing, slices, typed `int[]`, and `len`, `push`, `sort`, ... see below).
Functions are next.

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
| `--` | Stop option parsing |

## Development

`.clangd` reads `build/compile_commands.json`, so configure once before
opening the project in an editor. `.clang-tidy` and `.clang-format` are at the
repository root.

See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for the layout, the memory
model, and how to add a builtin.
