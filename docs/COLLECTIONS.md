# Collections

Arrays and dicts are mutable and passed by reference. An array owns each element; a
dict owns each key and value. Copying a value into a container adds a reference;
dropping a value removes one. Only `nil` and `false` are falsy, so `[]` and `{}`
are truthy — an empty collection is still "something" as far as conditions are
concerned.

```lizard
let xs = [1, 2, 3]
let ys: int[] = [4, 5]
println(xs[-1])       # 3
xs[0] = 10
println(xs[1:3])      # [2, 3]
println(xs + ys)      # [10, 2, 3, 4, 5]
```

```text
3
[2, 3]
[10, 2, 3, 4, 5]
```

Negative indexes count from the end. Slices (`xs[a:b]`, either bound optional)
build new arrays and clamp instead of failing; indexing out of range is a run-time
error. `==` compares arrays by content.

| Builtin | Effect |
| --- | --- |
| `len(x)` | items of an array |
| `push(xs, v)`, `pop(xs)` | append, or remove and return the last item |
| `insert(xs, i, v)`, `remove(xs, i)` | insert before index `i`, or remove and return item `i` |
| `clear(xs)`, `copy(xs)` | empty in place, shallow copy |
| `contains(xs, v)`, `index_of(xs, v)` | search by `==`; `index_of` gives `-1` when absent |
| `reverse(xs)`, `sort(xs)` | in place; `sort` needs all numbers or all strings, and is stable |
| `join(xs, sep)` | items to text, separated by `sep` |
| `range(n)`, `range(a, b)` | the ints `0..n-1` or `a..b-1` |
| `sum(xs)`, `min(xs)`, `max(xs)` | numbers only; `min` and `max` fail on an empty array |
| `map(xs, f)`, `filter(xs, f)` | a new array, or of the items for which `f(item)` is truthy |
| `reduce(xs, f, init)` | folds the items: `acc = f(acc, item)`, from `init` |
| `each(xs, f)` | calls `f(item)` for every item, returns `nil` |
| `find(xs, f)`, `any(xs, f)`, `all(xs, f)` | first item with a truthy `f(item)`, or some / every item |

`int[]`, `string[]`, `int[][]`, `(int | string)[]` and `int?[]` are types. An array
stored under such a type is checked once and then restricted to it — `push(ys,
"x")` fails on an `int[]`. An unannotated array holds anything.

## Dicts

A dict maps keys to values and keeps the order the keys were added in:

```lizard
let ages = {"ana": 20, "budi": 31}
ages["cici"] = 24
ages["ana"] = 21
println(ages)
println(ages["budi"], ages.len(), len(ages))
```

```text
{"ana": 21, "budi": 31, "cici": 24}
31 3 3
```

| Builtin | Effect |
| --- | --- |
| `len(d)`, `d.len()` | number of entries |
| `d.keys()`, `d.values()`, `d.items()` | new arrays of keys, values, or `[key, value]` pairs |
| `d.get(k)`, `d.get(k, default)` | the value, or `nil` / the default when absent |
| `d.has(k)` | whether the key is there |
| `d.remove(k)` | remove and return the value; an error when absent |
| `d.clear()`, `d.copy()` | empty in place, shallow copy |
| `d.merge(other)` | a new dict; `other` wins on a shared key |
| `d.is_empty()` | whether it has no entries |

Keys are `string`, `int` or `bool`, and the kinds never mix: `1`, `true` and
`"1"` are three different keys. `{1.5: 1}` and `{[]: 1}` are compile errors, and a
key that turns out to be one of those at run time fails with `a float cannot be a
dict key`.

If you index a dict with a missing key, Lizard stops you:

```lizard fail
let ages = {"ana": 20}
println(ages["nope"])
```

```
error: key "nope" not found
  = help: did you mean "ana"?
```

Use `d.get(k, default)` instead. `d[k] = v` adds the key; `d[k] += v` needs it to
exist already.

A dict is a reference and may not hold itself: storing one inside itself fails
instead of making a cycle. `d == other` compares by content, ignoring order.
`for k in d` walks the keys, `for k, v in d` the key and the value, both in order.
Changing the size during iteration is `dict changed size during iteration`.

If you try to index an array out of range:

```lizard fail
let xs = [1, 2, 3]
println(xs[5])
```

```
error: RangeError: index out of range
```

Check `i < len(xs)` first, or use a slice.

If you try to use a value that cannot be a dict key:

```lizard fail
let d = {[1, 2]: 1}
```

```
error: an array cannot be a dict key
```

(for a float key: `a float cannot be a dict key`.) Use a string, int, or bool key.

Arrays and dicts grow as needed; there is no fixed length limit beyond memory.
`min` / `max` fail on an empty array, and `d[k] += v` requires the key to exist
already.

Array and dict methods are in [Methods](METHODS.md). Ownership and cycles — why a
dict may not hold itself — are in [Memory](MEMORY.md). `int[]` and `dict[K, V]`
types are in [Types](TYPES.md).
