# Memory, ownership and links

Lizard frees memory by counting references, with one collector for the one kind of
cycle reference counting cannot free (a cycle a closure makes), and one feature for
references that should not own anything (`link`). This page explains what owns what,
what is refused, and how to choose.

## Ownership

Values that share an object **own** it: arrays, dicts, instances and closures keep
everything they hold alive.

- An array owns each element; a dict owns each key and value; an instance owns each of
  its fields; a closure owns the values its upvalues hold.
- Copying a value into a container adds a reference (`x = y`, `a.push(y)`, `a.k = y`);
  dropping a value, or letting it leave scope, removes one.
- An object is freed when its last owner lets go. Freeing a container frees what only it
  still held. Release is **iterative** (a `pending`/`draining` loop, see
  `src/core/memory.c`): a chain of a million owning `next` fields is freed without
  recursing on the C stack.

A `link` is the one value that does **not** own: see below.

## Cycles are refused when stored

Storing a value into a container that already reaches back to the container would close
a cycle with no outside owner, and reference counting could never free it. Lizard
refuses the store instead of leaking:

```lizard
class Node { pub next: any = nil }
let a = Node()
let b = Node(next: a)
a.next = b        # error: cannot store: this would create a reference cycle
```

The same check covers arrays (`a.push(a)`) and dicts (`a["x"] = a`). The message names
the field and points at the fixed-size alternative:

```
cannot store: this would create a reference cycle (through field 'next' of Node)
help: a value cannot contain itself; for a pointer back to the owner, use a link field (pub link parent: Node = nil)
```

## Skink: cycles through closures

A closure can still make a cycle: a method stores a lambda that captures `self` in one
of `self`'s fields, or stores a bound method. Skink, the cycle collector, finds those:

- It is triggered where a cycle usually closes: when a frame returns and a captured
  variable moves into an upvalue (`lz_vm_close_upvalues`), and every so often on
  allocation.
- It runs a bounded **trial** deletion from a suspect. A trial that finds a dead cycle
  frees it; one that finds the suspect still held leaves it on a watch list with backoff.
- A **full pass** runs when the live heap has grown enough (about twice what the last
  pass left), before `--max-heap` would refuse an allocation, and at the end of the
  program.

Links are never edges of a cycle: a trial does not follow a `link`, and freeing the
target of a link only empties the cell. So a parent/child tree with a link back to the
parent is freed by ordinary reference counting, without the skink.

## Links: references that do not own

`link(x)` builds a value that points at `x` without keeping it alive:

```lizard
let l = link(x)
println(l.alive)     # true
l.target             # x
x = nil
println(l.alive)     # false
println(l.target)    # nil
println(l)           # nil
```

A field declared `pub link name: T = value` stores the link; reading the field gives the
target (or `nil` once it is gone), and writing it accepts the target, `nil` or a link.
Assigning to a link field runs **no** cycle check and does not make the object hold its
target.

```lizard
class Node {
    pub name: string
    pub link parent: Node = nil
    pub children: Node[] = []
}

let root = Node(name: "root")
let kid = Node(name: "kid", parent: root)   # does not hold root
root.children.push(kid)                     # no cycle: root owns kid, kid only links root
```

- A link may point at an instance, an array, a dict, a closure or a function (and `nil`).
  Strings and scalar values are rejected: `link(5)` is a `TypeError`.
- `link(link(x))` is `link(x)`: a link is never linked.
- `==` compares what the links point at; `copy()` and `with()` copy the link (a fresh
  cell, same target); printing shows `<link Node>`, or `nil` when the target is gone.
- A dead link reads as `nil`; a method called through it is a run-time error.

## When the target dies

The VM keeps a small side table: target → the cells that point at it. Freeing a target
that any cell points at empties those cells (`lz_link_target_freed`) before its memory
goes, so no pointer can dangle, even when the allocator reuses the address. A target
with no link costs nothing beyond one bit in its header. Freeing a cell removes it from
the table. The table is a hash, so freeing N linked objects is linear, not quadratic.

## How to choose

| You have | Use |
|---|---|
| a container that must keep its contents alive | an ordinary field / element |
| a back-pointer to an owner, an observer, a cache entry | `link` (or a `link` field) |
| an id or a name you can look up again | a plain `int`/`string` |
| a cycle that is genuinely needed | restructure: a `link` breaks ownership; cycles through owning fields are refused |

## `--heap-stats`

`lizard --heap-stats program.lz` prints, on stderr, what the heap did:

```
heap: N allocations, peak B bytes, 0 objects (0 bytes) still live at exit
link: L cells alive, C emptied when their target was freed
skink: ... suspects, trials, dead cycles ...
skink: worst trial pause X ms (... total), worst full pass Y ms (... total)
```

The timing lines are measured only with `--heap-stats` (reading the clock on every trial
would otherwise cost more than the trial).

## Measurements

Run `bench/memory/run.sh [path/to/lizard]`. It runs each script in `bench/memory/` with
`--heap-stats` at a size and at twice the size, and flags a growth above ~2.5x when the
input doubles as superlinear. Work that is linear doubles.

A representative run (release build, one machine):

| benchmark | size N | 2N | growth |
|---|---|---|---|
| array_instances | ~300 ms | ~655 ms | 2.2x |
| list_append | ~150 ms | ~260 ms | 1.7x |
| list_prepend | ~28 ms | ~27 ms | 1.0x |
| tree (link parent) | ~950 ms | ~1.8 s | 1.9x |
| chain_drop | ~28 ms | ~25 ms | 0.9x |
| shared_container | ~31 ms | ~34 ms | 1.1x |
| big_live | ~770 ms | ~730 ms | 1.0x |

All are linear within noise. The tree benchmark is what caught the first design of the
link table (a linear scan per freed target); it was replaced by a hash of target → cells
before it was accepted.
