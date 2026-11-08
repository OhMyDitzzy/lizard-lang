# Control flow

A Lizard program runs top to bottom, and control flow is what changes that order.
`if` chooses between branches, `while` repeats while a condition holds, and `for`
walks over a collection or a range. `break` leaves a loop early; `continue` skips
to the next round. Only `nil` and `false` are falsy, so an empty array `[]` and an
empty dict `{}` are both truthy — a useful detail when you write conditions.

`switch` is also control flow; see [Syntax](SYNTAX.md) for the `switch` statement.

```lizard
let s = 0
let k = 0
while k < 10 {
  k = k + 1
  if k % 2 == 0 { continue }
  s = s + k
}
println(s)
```

```text
25
```

## Loops

| You want | Write |
| --- | --- |
| every item of an array or string | `for x in xs { ... }` |
| an item and its index | `for i, x in xs { ... }` |
| a plain counter, up or down | `for i in 0..n` / `for i in n..0 by -1` |
| a complex step or your own condition | `for let i = 0; i < n; i *= 2 { ... }` |
| repeat while a condition holds | `while cond { ... }` |

`a..b` visits `a` up to `b - 1`; `a..=b` includes `b`. The bounds and the step are
evaluated once, must be ints, and never overflow: `0..=INT64_MAX` ends after one
round of `INT64_MAX`. A step of 0 is an error. A range is not a value — `let r =
0..3` fails with `a range can only be used in a 'for' loop or after 'in'`.

```lizard
for x in [4, 7, 9] { print(x, " ") }
println()
for i, ch in "hé" { println(i, ch) }
for i in 0..=3 { print(i) }
println()
for i in 10..0 by -3 { print(i, " ") }
println()
for let i = 1; i < 100; i *= 2 { print(i) }
```

```text
4 7 9
0 h
1 é
0123
10 7 4 1
1248163264
```

Notice that `"hé"` walks one code point at a time: index 0 is `h`, index 1 is `é`.

## `break` and `continue`

`break` leaves the innermost loop. `continue` jumps straight to its next condition.
Both are statements without a value or label, and anything after them in the same
block does not run:

```lizard
let xs = [4, 7, 9, 12]
let i = 0
while i < len(xs) {
  if xs[i] % 2 == 0 { break }
  i = i + 1
}
println(i, xs[i])
```

```text
1 7
```

If you try `break` or `continue` outside a loop, Lizard stops you:

```lizard fail
fn f() { break }
```

```
error: 'break' outside of a loop
```

Use them only inside a loop. A lambda written inside a loop body cannot use `break`
or `continue` either, because the loop is not part of that function.

If you write a range with a step of 0:

```lizard fail
for i in 0..5 by 0 { print(i) }
```

```
error: step must not be zero
```

Use a non-zero step instead.

When the body of a `for` shrinks the array it is walking:

```lizard
let xs = [1, 2, 3, 4, 5]
for x in xs { xs.remove(0) }
println(xs)
```

The array's length is re-read every round, so the loop stops early once the array is
empty — no out-of-range access. What `xs` holds after the loop depends on how many
rounds ran.

Calls nest up to 2048 deep. A range's bounds and step must be ints — `let r = 0..3`
errors because a range is not a value. Every back edge, including the one `continue`
takes, is charged to the fuel limit, so `for ;; { }` stops when fuel runs out.

Error handling with `try`, `catch`, `finally`, and `throw` is covered in
[Errors](ERRORS.md). Array and dict methods you might use inside loops are in
[Methods](METHODS.md).
