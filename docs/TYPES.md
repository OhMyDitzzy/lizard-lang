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


## Function types

A function can be a value, and a value can have a type. `fn(int, string) -> bool` is
the type of a function that takes an `int` and a `string` and gives back a `bool`. It
works anywhere a type does: on a variable, a parameter, a field, or in an alias such as
`type Pred = fn(int) -> bool`.

```lizard
fn apply(f: fn(int) -> int, x: int) -> int {
    return f(x)
}

fn double(n: int) -> int { return n * 2 }
println(apply(double, 4))
```

```
8
```

Leave the `->` off and you say you do not care what comes back: `fn(int)` is a function
that takes one `int`. A plain `fn` still means any function at all. A function type
with a result swallows what follows it, so write `(fn(int) -> int)?` for "a function or
nil" and `(fn(int) -> int)[]` for a list of them.

The compiler knows the signature of every function you declare, so it checks them against
the type before the program runs. Parameters are checked the way a caller would see them
(what the caller passes must fit what the function takes), the result the other way round:

```lizard fail
fn apply(f: fn(int) -> int, x: int) -> int {
    return f(x)
}

fn shout(s: string) -> int { return s.len() }
println(apply(shout, 4))
```

```
error: type mismatch: argument 'f' of apply() expects fn(int) -> int, got fn(string) -> int
 --> /home/claude/t/main.lz:6:15
  |
6 | println(apply(shout, 4))
  |               ^^^^^ parameter 1 takes string, but it will be given int
  |
  = help: change the parameter to int, or the function type to take string

error: aborting due to 1 previous error
```

The count of parameters matters too, and the message says which side to change:

```lizard fail
fn apply(f: fn(int) -> int, x: int) -> int {
    return f(x)
}

fn add(a: int, b: int) -> int { return a + b }
println(apply(add, 4))
```

```
error: type mismatch: argument 'f' of apply() expects fn(int) -> int, got fn(int, int) -> int
 --> /home/claude/t/main.lz:6:15
  |
6 | println(apply(add, 4))
  |               ^^^ this takes 2 arguments, but 1 will be passed
  |
  = help: give the function 1 parameter, or change the parameters of the type

error: aborting due to 1 previous error
```

The check is only as sharp as what the compiler knows. A function that arrives in an
untyped variable is checked when it is stored, and then only for its number of parameters;
the parameter types are not tested at run time, and calls stay as fast as they were.

## Generic functions

A function that works the same for any element type can name that type. Put the type
parameters in angle brackets after the name:

```lizard
fn map<T, U>(xs: T[], f: fn(T) -> U) -> U[] {
    let out: U[] = []
    for x in xs { out.push(f(x)) }
    return out
}

println(map([1, 2, 3], fn(n: int) -> string { return $"<{n}>" }))
```

```
["<1>", "<2>", "<3>"]
```

Inside the body `T` and `U` are just `any`: the checks that run are the ones you wrote.
Their job is at the call. The compiler works out what each one stands for from the
arguments (arrays, dicts, function types, classes, unions such as `T?`) and carries it
into the result, so the result of `first([10, 20])` is an `int` as far as checking goes:

```lizard fail
fn first<T>(xs: T[]) -> T {
    return xs[0]
}

let n = first([10, 20])
println(n + 1)
let s: string = first([10, 20])
```

```
error: type mismatch: expected string, got int
 --> /home/claude/t/main.lz:7:17
  |
7 | let s: string = first([10, 20])
  |                 ^^^^^^^^^^^^^^^
  |
  = help: convert the value: string(first([10, 20]))

error: aborting due to 1 previous error
```

If it cannot tell — the argument is untyped, say — the result is `any`, which is not an
error. If the arguments disagree, it is:

```lizard fail
fn same<T>(a: T, b: T) -> T { return a }
println(same(1, "a"))
```

```
error: conflicting types for T in a call of same(): int and string
 --> /home/claude/t/main.lz:2:17
  |
2 | println(same(1, "a"))
  |                 ^^^ this is string
  |
  = note: T was already int from an earlier argument
  = help: pass values of one type, or write the parameters as int | string instead of T

error: aborting due to 1 previous error
```

Type parameters belong to functions and methods; there are no generic classes yet, and no
constraints. A generic function works as a method too: `[1, 2].first()` passes the array
as `xs`, and `extend` blocks may declare generic methods.

## Narrowing

A check on a variable tells the compiler more about it for as long as the check holds. With
`let name: string? = ...`, inside `if name != nil Ellipsis` the variable is a `string`:

```lizard
let name: string? = "Ada"
if name != nil {
    println(name.len())
}
```

```
3
```

The forms it understands are `x != nil`, `x == nil`, `x is T`, `x is not T`, `!(...)`,
chains with `and`, and `or` when both sides speak about the same variable. The `else`
branch gets the opposite, and so does an `else if` chain. A check that leaves the
function narrows what follows it:

```lizard
fn shout(name: string?) {
    if name == nil { return }
    println(name.len())
}
shout("Ada")
shout(nil)
```

```
3
```

Narrowing applies to local variables, parameters and top-level variables, not to fields or
indexes. It ends at the next assignment to the variable. It is not used for a variable that
some function body assigns anywhere, and it does not reach into closures, because they can
run at any time. Code that never narrows behaves as it did.

What narrowing does is catch what is certain. Where the check proves the variable is nil,
using it as a string is an error before the program runs:

```lizard fail
fn greet(name: string?) {
    if name == nil {
        name.len()
    }
}
```

```
error: nil has no method 'len'
 --> /home/claude/t/main.lz:3:14
  |
3 |         name.len()
  |              ^^^ name is nil here
  |
help: check `name != nil` first
  |
3 |         if name != nil { name.len() }
  |         ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

error: aborting due to 1 previous error
```

Without a check the call is not proven wrong, and keeps failing at run time as before.

## What the compiler knows and what runs

`let x = 0` has no annotation, so `x` is `any` when the program runs and nothing about it is
ever checked at run time. A function without `->` gets the union of what its `return`s
give (with `nil` if its end can be reached) as its result, used for checking and for the
tools that read a module's interface, never for a run-time test. If working it out would
need the function itself (recursion), or one `return` is untyped, the result stays `any`:

```lizard fail
fn pick(c) {
    if c { return 1 }
    return "one"
}
let b: bool = pick(true)
```

```
error: type mismatch: expected bool, got int | string
 --> /home/claude/t/main.lz:5:15
  |
5 | let b: bool = pick(true)
  |               ^^^^^^^^^^

error: aborting due to 1 previous error
```

