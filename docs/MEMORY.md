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

## Small object storage

Each heap pools object blocks up to 256 bytes in classes rounded up to 16 bytes.
On the audited 64-bit build, string and closure headers are 56 bytes, upvalues and
arrays are 88, instances and bound methods are 80, dicts are 120, coroutines are
192 and functions are 240. Variable-size payloads can take an object above the
cutoff. Class objects are 1360 bytes and always use `malloc`.

The first small object allocates a 64 KiB chunk. Further small objects use its
remaining space or a free block in their size class. The empty heap allocates no
chunks. Larger objects still use `malloc` and `free`. Chunks stay until
`lz_vm_free` or `lz_heap_destroy`, even when empty. Keeping them avoids allocator
traffic during repeated bursts of temporary objects. The pool does not try to
return empty chunks to the system while the VM lives.

`--max-heap` and heap statistics still count the exact requested object size, not
the rounded class size, and retain the existing charges for owned buffers.
Physical memory can exceed that count because of rounding, chunk headers, unused
chunk space and blocks kept for reuse. Object allocation hooks, finalization,
link invalidation and Skink run in their existing order.

Only `lz_heap_alloc` object blocks use this pool. Objects never use `realloc`,
and their header keeps the requested size so freeing identifies their class.
Array and dict storage, coroutine stacks, chunks of bytecode and native API
`lzn_alloc` buffers keep their own allocation and reallocation paths. Native
class finalizers are called as before, before the object block is recycled.

ASan disables the pool automatically for both GCC's `__SANITIZE_ADDRESS__` and
Clang's address-sanitizer feature. `-DLZ_NO_POOL` disables it explicitly. Use
`--disable-pool` with configure, or `--no-pool` with `tools/build-plain.sh`.
Poison mode fills pooled free blocks with `0xDD`, reserving the first pointer for
the free list. Allocation checks the remaining bytes before reusing a block and
aborts with `pooled object modified after free` if they changed. Nonpooled
allocations keep the existing poison quarantine.

The local allocation audit wrapped libc `malloc` and `free` with the linker's
`--wrap` option. Allocating and releasing a 61-byte object 100,000 times took
100,000 calls to each with the pool disabled and one call to each with it enabled,
including heap destruction. Both runs recorded 100,000 logical allocations and
zero live bytes at the end. This measures allocator traffic, not an Ir percentage.
The randomized lifetime test checks 100,000 operations and keeps one verified
payload alive across as many as 2640 intervening operations. The size-boundary
test checks every requested size from the object header through 320 bytes at an
exact two-object heap limit. Poison tests also corrupt a freed pooled block in a
child process and require the reuse check to reject it.

For example, use separate build directories to compare both allocators:

```sh
sh tools/build-plain.sh check --builddir=build-pool
sh tools/build-plain.sh check --no-pool --builddir=build-no-pool
sh tools/build-plain.sh check --poison --builddir=build-pool-poison 'CFLAGS=-O1 -g'
```

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
cannot store: this would create a reference cycle
help: make the field a link: it points at an object without owning it
    pub link next = nil
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

## Invariants of links

1. **A link cell never points at a freed object.** Every cell with a target is registered in the
   link table (`lz_link_check` verifies this against the heap).
2. **The hook runs at the moment the refcount reaches 0, on every path.** `lz_object_release` empties
   the links to the object before it checks `draining`, so objects freed as children (array items,
   fields, dict values, upvalues, Skink victims) are covered like the outermost one. The hook only
   edits the table and the cells; it releases nothing, so it is safe in the middle of a drain.
3. **No unregistered cell exists.** `lz_link_track` returns `false` when the row cannot be made;
   the cell then has no target and the caller reports out of memory. `lz_link_forget` changes the
   counts only when it finds the cell in its row.
4. **A cell is not the object it points at.** It matches no class, array, dict, function or scalar
   type (only `any`, and link fields, which check their target). Operators, `for`, calls, indexing
   and builtin functions refuse it with `TypeError: a link has to be read with .target first, got
   link`. Allowed: `.target`, `.alive`, fields and methods of a link to an instance (`self` is the
   object), `==`, `identical`, `is`, printing, and storing it in a variable, array, dict or link
   field. `copy()` and `with()` through a link are errors.
5. **`lz_heap_destroy` frees objects without finalizing them.** Cells may dangle for that moment;
   nothing reads a cell or a target afterwards (`lz_link_table_free` frees rows only).

### Tools

- **Poison mode:** `./configure --enable-poison`, then `make check`. Freed blocks are
  filled with 0xDD and kept in a 4096-block quarantine, and `retain`/`release`/`deref` assert that
  the object is not poisoned. `tests/poison_selftest.sh` puts the old bug back
  (`-DLZ_TEST_REINTRODUCE_LINK_BUG1`) and expects `test_links` to fail.
- **Model fuzzer:** `tests/fuzz_links.c` (`fuzz_links [seeds] [first-seed] [-v [max-ops]]`, or
  `LZ_FUZZ_SEEDS`). It generates programs, predicts every `.alive` with a small model, and checks
  `lz_link_check` and `live_objects == 0`. For objects that only a closure cycle could still hold,
  the model accepts either answer, because Skink's schedule is not modelled.
- **CI:** `.github/workflows/sanitizers.yml` (ASan/UBSan/LSan, poison, Valgrind) and
  `tools/valgrind-check.sh`.

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
## Allocation profiling

Immutable strings start with an unset zero hash. `lz_string_hash` computes it on the first
key or type-hash use and retains it. `lz_hash_bytes` maps a genuine zero result to one so
the sentinel cannot be confused with a computed hash. Concatenation, case mapping and
template construction therefore skip hashing when the result is never used as a key.

The allocation profile in `bench/profile.sh` runs dhat with `LZ_POOL=off` in builds that include
the object pool. That makes dhat report individual heap objects instead of only the backing pool
chunks. This switch affects allocation strategy, not the heap limit or language behavior.
