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

### Conditions as expressions

`if` in an expression position gives a value: after `=`, `(`, `,`, `[`, `return`, `=>`, an
operator, or inside `{}` of a template string.

```
let label = if n > 0 { "positive" } else { "non-positive" }
let sign  = if n > 0 { 1 } else if n < 0 { -1 } else { 0 }
let abs   = fn(x) => if x < 0 { -x } else { x }
println($"{if n > 0 { "yes" } else { "no" }}")
println(if n > 0 { 2 } else { 3 } * 10)      # 20: the operator applies to the whole if
```

* Each branch holds exactly one expression; a newline after `{` and before `}` is fine.
  A statement there is an error: `a branch of an 'if' expression holds one expression,
  not a statement`.
* `else` is required, and an `else if` chain must end in a plain `else`:
  `an 'if' expression needs an 'else'`.
* Only the branch taken runs, and the result's type is the union of the two branches, so
  `if a { 1 } else { "x" }` is an `int | string`.
* `if` at the start of a statement is still the statement form, unchanged.
* `if` is a prefix expression: it binds looser than everything to its right, so an
  operator after the last `}` applies to the whole `if`. `if a { 1 } else { 2 } + 3` is
  `(if a { 1 } else { 2 }) + 3`. Use parentheses when in doubt.

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

### Loops

`for x in ...` walks an array, a string or a range; `for let i = 0; i < n; i += 1
{ ... }` is the free form; `while (condition) { ... }` runs the body while the
condition is truthy.

| You want | Write |
| --- | --- |
| every item of an array or string | `for x in xs { ... }` |
| an item and its index | `for i, x in xs { ... }` |
| a plain counter, up or down | `for i in 0..n { ... }` / `for i in n..0 by -1 { ... }` |
| a complex step, two counters, your own condition | `for let i = 0; i < n; i *= 2 { ... }` |
| repeat while a condition holds, with no counter | `while cond { ... }` |

```
for x in [4, 7, 9] { print(x, " ") }         # 4 7 9
for i, ch in "hé" { println(i, ch) }         # 0 h / 1 é
for i in 0..3 { print(i) }                   # 012
for i in 0..=3 { print(i) }                  # 0123
for i in 10..0 by -3 { print(i, " ") }       # 10 7 4 1
for let i = 1; i < 100; i *= 2 { print(i) }  # 1248163264
```

* `a..b` visits `a` up to `b - 1`; `a..=b` includes `b`. The bounds and the step
  are evaluated once, must be ints, and are never overflowed: `0..=INT64_MAX`
  ends after one round of `INT64_MAX`. A step of 0 is an error. A range is not a
  value: `let r = 0..3` is `a range can only be used in a 'for' loop`.
* An array's length is read again every round, so a loop over an array the body
  grows walks the new items and one over an array it shrinks stops early. A
  string is walked one code point at a time without building an array of them.
* The loop variables are fresh each round and cannot be assigned
  (`cannot assign to the loop variable 'x'`). A closure made in the body keeps
  that round's value.
* A C-style loop's variables live only in its header and body. `continue` runs
  the step; `break` leaves the loop. Closures made in the condition or the step
  are not given a fresh variable.
* Every back edge, including the one `continue` takes, is charged to the fuel
  limit, so `for ;; { }` stops when one is set.


`break` leaves the innermost loop; `continue` goes straight to its next
condition. Both are statements of their own, without a value or a label, and
may stand inside an `if`. Anything after them in the same block does not run.

```
let xs = [4, 7, 9, 12]
let i = 0
while i < len(xs) {
  if xs[i] % 2 == 0 { break }   # the first even item
  i = i + 1
}
println(i, xs[i])               # 1 7

let s = 0
let k = 0
while k < 10 {
  k = k + 1
  if k % 2 == 0 { continue }    # skip the even numbers
  s = s + k
}
println(s)                      # 25
```

* `break` and `continue` outside a loop are compile errors: `'break' outside of
  a loop`. So is one inside a function or lambda written in a loop body, since
  the loop is not part of that function.
* A `continue` jumps to the condition through the same back-edge as the end of
  the loop, so a `while true { continue }` still runs out of fuel when one is set.

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

### Dicts

A dict maps keys to values and keeps the order the keys were added in.

```
let ages = {"ana": 20, "budi": 31}
ages["cici"] = 24               # added at the end
ages["ana"] = 21                # overwritten in place: still first
println(ages)                   # {"ana": 21, "budi": 31, "cici": 24}
println(ages["budi"], ages.len(), len(ages))   # 31 3 3
```

| Builtin | Effect |
| --- | --- |
| `len(d)`, `d.len()` | number of entries |
| `d.keys()`, `d.values()`, `d.items()` | new arrays of the keys, of the values, or of `[key, value]` pairs |
| `d.get(k)`, `d.get(k, default)` | the value, or `nil` / the default when the key is absent |
| `d.has(k)` | whether the key is there |
| `d.remove(k)` | remove and return the value; an error when the key is absent |
| `d.clear()`, `d.copy()` | empty in place; a shallow copy |
| `d.merge(other)` | a new dict: `other` wins on a shared key, and the key keeps its place |
| `d.is_empty()` | whether it has no entries |

* Keys are `string`, `int` or `bool`, and the kinds never mix: `1`, `true` and
  `"1"` are three different keys. `{1.5: 1}` and `{[]: 1}` are compile errors,
  and a key that turns out to be one of those at run time fails with
  `a float cannot be a dict key`.
* A literal key written twice is a compile error; a key that is the same only at
  run time (`{k: 1, k: 2}`) stores the last value.
* `d[k]` on a missing key is a run-time error, with a suggestion from the keys
  that are there: `key "dewi" not found` / `did you mean "ana"?`. `d[k] = v`
  adds the key; `d[k] += v` needs it to exist already and says
  `use d.get(k, 0) + v` when it does not.
* A dict is a reference, like an array, and a dict may not hold itself: storing
  one inside itself fails instead of making a cycle.
* `d == other` compares by content, ignoring the order. Display quotes the
  strings and nests, and `{d}`, `{d=}` and `{d:s}` in a template work as they do
  for arrays. Only an empty dict and `false` are falsy.
* `dict`, `dict[K, V]` and `dict[string, int]` are types, and a dict stored under
  one is restricted to it, so a later `d["b"] = "no"` on a `dict[string, int]`
  fails. An unannotated `let d = {...}` is not restricted.
* `for k in d` walks the keys, `for k, v in d` the key and the value, both in
  order. Changing a value during the walk is fine; changing the size is
  `dict changed size during iteration`.
* A `{` at the start of a statement is still a block, so a dict literal after
  `in` needs parentheses: `for k in (d) { ... }`, and in a template
  `$"{ {"a": 1}["a"] }"` needs the space. `{{` is still the brace escape.

```
let counts = {}
for word in "a b a".split(" ") { counts[word] = counts.get(word, 0) + 1 }
for word, n in counts { println(word, n) }   # a 2 / b 1
```

More in `examples/`. Run one with `build/bin/lizard examples/fizzbuzz.lz`.

### Operators

Logic can be written with symbols or words; both mean the same thing:

| Symbol | Word |
| --- | --- |
| `&&` | `and` |
| `\|\|` | `or` |
| `!x` | `not x` |
| `==` | `equal` |
| `!=` | `not equal` |

**Compound assignment** writes `x op= v` for `x = x op v`, with every binary operator
the language has: `+=` `-=` `*=` `/=` `//=` `%=` `&=` `|=` `^=` `<<=` `>>=`. It works on
locals, globals, captured variables and array items (`xs[i] += 1`, `grid[r][c] += 1`).
The target is evaluated once, and the result is checked against the variable's type
exactly as `=` would check it.

```
let total = 0
total += 2 * 3        # 6
let flags = 0b1000
flags &= ~1           # 8
flags <<= 1           # 16
xs[i] += 1
```

```
if i != 2 && i != 3 { ... }
if not (i equal 2) and i not equal 3 { ... }
```

`not`, `equal` and `and`/`or` are keywords, so they cannot be used as names.

Bitwise operators work on `int` only and always give an `int` (64-bit two's complement):
`a & b`, `a | b`, `a ^ b`, `~a`, `a << n`, `a >> n`. They have no word forms.

**Precedence**, from the strongest to the weakest. Binary operators are left-associative, and
a newline after one is ignored.

| Level | Operators |
| --- | --- |
| unary | `-` `!` `~` (and `not`) |
| multiplicative | `*` `/` `//` `%` |
| additive | `+` `-` |
| shift | `<<` `>>` |
| bitwise and | `&` |
| bitwise xor | `^` |
| bitwise or | `\|` |
| comparison | `<` `<=` `>` `>=` |
| equality | `==` `!=` |
| logical and | `and` `&&` |
| logical or | `or` `\|\|` |

So `flags & 4 == 4` means `(flags & 4) == 4` (unlike C), and `1 + 2 << 3` is `24`.

```
let flags = 0b1010_0101
println(flags & 0xF0, flags | 1, flags ^ 0xFF, ~flags)    # 160 165 90 -166
println(1 << 4, -8 >> 1, n & 1 == 1)                      # 16 -4 true when n is odd
```

- Anything that is not an int is an error: `operator '&' needs two ints, got float and int`.
  For `bool` it adds a hint (`use 'and' for logic`). The error is found at compile time when the
  static types rule it out, and when the program runs when the type is `any`.
- A shift count must be in `0..63`, otherwise `shift count must be 0..63, got 64` at run time.
- `>>` is an arithmetic shift (the sign is kept), and there is no `>>>`.
- **`<<` never reports overflow.** It drops the bits that fall off the left end, so
  `1 << 63` is `-9223372036854775808` and `3 << 63` is the same. This is the one exception to
  the rule that integer overflow is an error (`+`, `-`, `*` still fail on overflow).
- **`^` is XOR, not a power.** `2 ^ 3` is `1`. For powers use `2.pow(3)`.
- In a type, `|` is still a union (`let x: int | string = a | b` works: the first is a type, the
  second an expression).

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
| int | `bit_count()` (1 bits in the 64-bit form, so `(-1).bit_count()` is 64), `bit_length()` (bits the magnitude needs, `0` gives 0), `bit(n)` (is bit `n`, 0..63, set) |

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
