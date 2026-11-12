# Syntax

A Lizard source file runs from top to bottom, one statement at a time. It is built
from expressions (things that produce a value, like `1 + 2` or `$"hi {name}"`),
statements (things that do something, like `let`, `if`, `while`, `for`, blocks), and
declarations (things that introduce a name, like `fn`, `class`, `trait`, `error`,
`type`).

Comments start with `#` and run to the end of the line. `let` declares a mutable
variable; `const` declares one that cannot be reassigned. A block `{ ... }` groups
statements and introduces a scope, so a variable declared inside a block is not
visible outside it.

```lizard
const GREETING = "Hello" + ", " + "Lizard"
let name: string = "Ana"
let i: int = 1
while i <= 3 {
  println($"{GREETING} {name}, round {i}")
  i = i + 1
}
```

```text
Hello, Lizard! Ana, round 1
Hello, Lizard! Ana, round 2
Hello, Lizard! Ana, round 3
```

## Imports

`import util`, `import util as u`, `import tools.format` and `from util import a, b as c` bring in the
`pub` names of another file. They stand at the top of the file, before any other statement. `import`,
`from` and `as` are only special in that position, so they are still fine as variable names. See
[MODULES.md](MODULES.md).

## Top-level declarations and `pub`

`fn`, `const`, `let`, `class`, `trait`, `error` and `type` can stand at the top of a file, and
`pub` may go in front of any of them except `let`. It marks what other code may use. Without modules it
changes nothing yet, but the compiler records it: every `pub` declaration, with its signature, goes
into the interface of the file, ready for the day another file asks for it.

```lizard
pub fn add(a: int, b: int) -> int => a + b
pub const MAX = 10
pub class Point {
  pub x: int
  pub y: int = 0
}
println(add(Point(1, 2).x, MAX))
```

```text
11
```

`pub` never goes in front of `let`. A variable can change, so other code could not rely on it;
what you share should stay the same (`const`), or be reached through a function or a class:

```lizard fail
pub let limit = 10
println(limit)
```

```
error: 'pub' cannot be used on 'let'
 --> ...
  |
1 | pub let limit = 10
  |     ^^^ a variable can change, so other code could not rely on it
  |
  = note: `pub` is for things that stay the same, or that you reach through a function
help: this variable never changes, so declare it with `const`
  |
1 | pub const limit = 10
  |     ~~~~~
```

When the variable is given a new value somewhere in the file, the compiler cannot suggest
`const`; keep the variable private and share it through a function instead:

```lizard
pub const MAX = 10
let calls = 0
pub fn count() => calls
fn tick() {
  calls += 1
}
tick()
println(MAX, count())
```

```text
10 1
```

`pub` belongs on top-level declarations only. Inside a function or a block it is refused, and so
is `pub` in front of something that declares nothing, like `extend` or a `switch`:

```lizard fail
fn outer() {
  pub fn helper() => 1
  return helper()
}
```

```
error: `pub` only works on top-level declarations
 --> ...
  |
2 |   pub fn helper() => 1
  |   ^^^ this is inside a function
  |
help: `pub` marks something other code can use, so it goes on top-level declarations
```

What a `pub` declaration shows must be nameable from outside. A `pub` function whose signature
uses a class (or trait, alias or error) that is not `pub` is an error, because code outside
could not write that type:

```lizard fail
class Hidden {
  pub n: int
}
pub fn make() -> Hidden => Hidden(1)
```

```
error: `make` is pub, but its signature uses `Hidden`, which is not
 --> ...
  |
4 | pub fn make() -> Hidden => Hidden(1)
  |                  ^^^^^^ `Hidden` is not `pub`
  |
  = note: code outside could not name this type
help: make `Hidden` public
```

Private fields and methods of a `pub` class are not part of the interface, so they may use any
type. Built-in types never need `pub`.

## Operators and precedence

Binary operators group from left to right, and a newline after an operator is
ignored. Precedence runs from tightest to loosest:

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
| fallback | `??` |

Because of this, `flags & 4 == 4` means `(flags & 4) == 4` — unlike C, where `&`
binds looser than `==`. And `1 + 2 << 3` is `24`, not `16`.

`not`, `equal`, `and`, and `or` are keywords, so they cannot be used as names.
Bitwise operators work on `int` only and always produce an `int`.

## `in`, `not in` and `??`

`x in c` checks whether `x` is in `c`, but what "in" means depends on `c`: an item
search for an array, a key search for a dict, a substring search for a string, and
a range comparison for a range. `not in` is a single operator — `not` and `in`
separated by spaces; `!in` does not exist.

`a ?? b` is `b` when `a` is `nil`, and `a` otherwise. Only `nil` counts, so
`false ?? 5` is `false`, not `5`. It is the weakest binary operator, below `or`.

```lizard
let ages = {"ana": 20, "budi": 31}
println("ana" in ages, "x" not in ages, 25 in 18..=65)
let n = ages.get("ceci") ?? "anon"
```

```text
true false true
anon
```

## `switch`

`switch` picks one arm by comparing a subject with the items of each `case`. At the
start of a statement it is a statement; everywhere else (after `=`, `return`, inside
a template, and so on) it is an expression:

```lizard
switch day {
    case 1 => "Monday"
    case 6, 7 => "Weekend"
    else => "Weekday"
}
```

Arms are tried top to bottom, the first match wins, and there is no fallthrough. A
range item like `4..=9` matches a numeric subject. Without a subject, each case is
a plain condition instead.

## `if` as an expression

`if` can appear in expression position (after `=`, `(`, `,`, `[`, `return`, `=>`,
an operator, or inside `{}` of a template). Each branch holds exactly one
expression, `else` is required, and the result type is the union of the branches:

```lizard
let sign = if n > 0 { 1 } else if n < 0 { -1 } else { 0 }
println($"{if n > 0 { "yes" } else { "no" }}")
```

So `if a { 1 } else { "x" }` has type `int | string`. An `if` at the start of a
statement is still the statement form, unchanged.

If you add values of different kinds, Lizard stops you:

```lizard fail
println(1 + "a")
```

```
error: cannot add: int and string
 --> ...
   |
3 | println(1 + "a")
   |     ^~~~~~~~~~~~ `int` and `string` cannot be added
```

Convert explicitly — `1 + int("a")` — or use interpolation, `$"{1}{a}"`.

If you use `in` with something it cannot search:

```lizard fail
println(1 in 3.14)
```

```
error: cannot use 'in' with float
```

Search an array, dict, string, or range instead.

And if a branch of an `if` expression holds a statement instead of an expression:

```lizard fail
let x = if true { let y = 1 } else { 2 }
```

```
error: a branch of an 'if' expression holds one expression, not a statement
```

Put an expression in each branch — `if true { 1 } else { 2 }`.

Calls nest up to 2048 deep; beyond that the program fails with the ordinary depth
error. Integer `+` / `-` / `*` fail on overflow, but `<<` never reports overflow —
it simply drops the bits that fall off the left end. A shift count must be `0..63`,
otherwise you get `shift count must be 0..63, got N` at run time.

You'll find loops and conditionals in [Control flow](CONTROL_FLOW.md), type
annotations in [Types](TYPES.md), and methods called on values in
[Methods](METHODS.md).
