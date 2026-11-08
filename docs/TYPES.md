# Types

Every Lizard value has a kind: `nil`, `bool`, `int`, `float`, `string`,
`function`, `array`, `dict`, `error`, or `object` (an instance of a class). A type
is the set of values a variable, parameter, array item, dict value, or field may
hold. Types are checked at compile time when the compiler can prove a mistake, and
at run time where it cannot — Lizard is gradually typed, not optionally typed.

| Type | Means |
| --- | --- |
| `int` | a 64-bit integer |
| `float` | a 64-bit floating point number |
| `number` | `int` or `float` |
| `bool` | `true` or `false` |
| `nil` | absence |
| `string` | UTF-8 text |
| `function` | a function, closure, or bound method |
| `array` | an array |
| `dict` | a dict |
| `error` | an error value |
| `object` | an instance of a class |
| `any` | every value |
| `never` | no value |

`int | string` is a union: a value of either kind. `"r" | "w"` is a union of string
literals: only exactly those strings. `int?` is shorthand for `int | nil`.

A `type` alias names a type: `type Mode = "r" | "w"`. Array types are written
`int[]`, `string[]`, `(int | string)[]`, `int?[]`; dict types `dict`,
`dict[K, V]`, `dict[string, int]`. A class name works as a type: `fn f(p: Point)`.

```lizard
type Mode = "r" | "w"
let mode: Mode = "r"
let name: string? = nil
name = "Lizard"
let xs: int[] = [1, 2, 3]
let d: dict[string, int] = {"ana": 20}
let n: int = int("12") + 1
let half: float = n / 2
println(mode, name, len(xs), d["ana"], n, half)
```

```text
r Lizard 3 20 13 6.5
```

`int(x)`, `float(x)`, `string(x)`, `bool(x)` convert a value. Parsing a string as a
number fails with a `ValueError` when the text is not a number. Conversions the
compiler can prove wrong are compile-time errors; conversions through `any` are
checked when the program runs.

If you store the wrong kind in a typed variable, Lizard stops you:

```lizard fail
let n: int = "5"
```

```
error: TypeError: ... expects int, got string
```

Convert explicitly — `let n: int = int("5")`.

If an operator gets the wrong kind:

```lizard fail
println(1 + "a")
```

```
error: cannot add: int and string
```

Use `1 + int("a")`, or `$"{1}{a}"`.

If an index is out of range:

```lizard fail
let xs = [1, 2, 3]
println(xs[5])
```

```
error: RangeError: index out of range
```

Check `i < len(xs)` first, or use a slice — slices clamp instead of failing.

If you use `nil` as a value:

```lizard fail
let name: string? = nil
println(name.len())
```

```
error: ... nil has no method 'len' (it works on string)
```

Check `name == nil` first, or guard the call so it only runs when `name` is not nil.

A written union may hold at most 8 literals (`LZ_TYPE_MAX_LITERALS`). `int` is
64-bit; `+` / `-` / `*` fail on overflow, `<<` never does. `int[]` / `dict[K, V]`
restrict their contents once annotated — `push(ys, "x")` fails on an `int[]`.

Array and dict types are in [Collections](COLLECTIONS.md). Class names as types and
`is` are in [Classes](CLASSES.md). The precedence of `|` in types versus
expressions is in [Syntax](SYNTAX.md).
