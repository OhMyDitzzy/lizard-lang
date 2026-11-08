# Methods and callbacks

`receiver.name(args)` is just `name(receiver, args)`: any function whose first
parameter is the receiver can be written as a method call. Builtins and your own
functions work the same way. A callback receives only the item; the array is visited
up to the length it had when the call began, so a callback that pushes to the array
cannot loop forever, and items it removes are simply not visited.

```lizard
let nums = [1, 2, 3, 4, 5, 6]
nums.filter(fn(n) => n > 2).map(fn(n) => n * 10)
nums.reduce(fn(acc, n) => acc + n, 0)
fn double(n) { return n * 2 }
5.double()
```

```text
[30, 40, 50, 60]
21
10
```

A chain can continue on the next line:

```lizard
nums
  .filter(fn(n) => n > 2)
  .len()
```

Strings, numbers, and arrays each have their own methods, called like
`value.name(args)` with snake_case names.

| For | Methods |
| --- | --- |
| every value | `to_string()`, `to_bool()` |
| int, float, string | `to_int()`, `to_float()` (a string is parsed) |
| string | `to_upper()`, `to_lower()`, `trim()`, `trim_start()`, `trim_end()`, `split(sep)`, `contains(s)`, `starts_with(s)`, `ends_with(s)`, `replace(old, new)`, `repeat(n)`, `index_of(s)`, `chars()`, `slice(from, to)`, `is_empty()` |
| array | `first()`, `last()` (`nil` when empty), `is_empty()`, and the functions above (`xs.len()`, `xs.contains(v)`, `xs.join(sep)`, …) |
| int, float | `abs()`, `floor()`, `ceil()`, `round()`, `sqrt()`, `pow(n)`, `min(other)`, `max(other)` |
| int | `bit_count()` (1 bits in the 64-bit form, so `(-1).bit_count()` is 64), `bit_length()` (bits the magnitude needs, `0` gives 0), `bit(n)` (is bit `n`, 0..63, set) |

```lizard
"  Hello, Lizard ".trim().to_lower()
"a,b,c".split(",")
"héllo".slice(1, 3)
2.5.round(), 7.pow(2), 16.sqrt()
nums.map(fn(n) => n.to_string()).join("-")
```

```text
"hello, lizard"
["a", "b", "c"]
"él"
3, 49, 4.0
1-2-3-4-5-6
```

Text is UTF-8: lengths and positions count characters, and searching works on bytes.
`to_upper`/`to_lower` use Unicode's simple case mapping, one character to one
(`é` to `É`, `ж` to `Ж`, `α` to `Α`), so `ß` stays `ß` — there is no locale-specific
rule. `trim` removes Unicode white space, including the no-break space. `round`
rounds halves away from zero, and `floor`, `ceil`, and `round` give an int. `slice`
takes negative positions and clamps, like an array slice.

If you call a method that does not exist on that kind:

```lizard fail
5.to_lower()
```

```
error: int has no method 'to_lower' (it works on string)
```

Convert first — `string(5).to_lower()` — or use a method that exists on `int`.

If you call a method on `nil`:

```lizard fail
let x: string? = nil
x.len()
```

```
error: nil has no method 'len' (it works on string)
```

Check `x == nil` first, or guard the call so it only runs when `x` is not nil.

A callback that mutates the array it visits does not loop forever — the array is
visited only up to the length it had when the call began, so newly pushed items are
not visited:

```lizard
let xs = [1, 2, 3]
xs.each(fn(x) { xs.push(x) })
```

A kind that has no method of that name fails with `int has no method 'to_lower' (it
works on string)`. A function or variable of your own with the same name always wins
over a method. Fields (`value.name` without a call) do not exist yet.

Callbacks run on the reference interpreter and may nest 128 deep. An error inside a
callback points at the failing line there, and the stack trace goes through to the
call that started it. `min` / `max` fail on an empty array.

Functions are values, and any function can be a method — see [Functions](FUNCTIONS.md).
Array and dict methods are in [Collections](COLLECTIONS.md). String methods are in
[Strings](STRINGS.md). Methods defined in a class are in [Classes](CLASSES.md).
