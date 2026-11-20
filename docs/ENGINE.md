# Engines

Lizard runs bytecode on one of two engines. They implement exactly the same
language and differ only in speed.

| Engine | Where | What it is |
| --- | --- | --- |
| `ref` | `src/core/run.c` | The reference interpreter: one `switch`, no tricks. It *defines* what every instruction does. |
| `ge` (default) | `src/engine/` | The Gecko Engine: a faster interpreter for the same bytecode. |

The reference dispatch loops keep the active value and frame buffer pointers in local
variables for their whole activation. A nested coroutine resume runs a separate loop and
restores the caller's context before returning, including on failure. Yield exits its loop,
so a later resume reads fresh pointers. A new context-switching path must preserve this
contract. Helpers outside the loop still resolve the active pointers when called. Handler
storage is only read on the slow try and unwind paths. Fuel and instruction counts remain
in the VM and are updated exactly once per bytecode instruction.

### Reference dispatch audit on AArch64

A local audit used Clang 21.1.8 with `-std=c11 -O2 -DNDEBUG -g -S`, without LTO.
It compared `f392115` (before coroutines), `7b04970` (the stage 3 starting point),
and the activation-local pointers. These are instruction counts along the successful
int path from the opcode's switch target to the shared status check. They exclude
dispatch, error branches and the bodies of called helpers. `INDEX` uses a positive
array index, `CALL` enters an ordinary closure, and `RETURN` leaves a function.

| Handler | Before coroutines | Stage 3 starting point | Local pointers and scalar stores |
| --- | ---: | ---: | ---: |
| ADD | 30 | 30 | 25 |
| LESS | 28 | 28 | 29 |
| GET_LOCAL | 23 | 22 | 23 |
| SET_LOCAL | 34 | 32 | 33 |
| JUMP_IF_FALSE | 26 | 25 | 25 |
| LOOP | 11 | 11 | 11 |
| INDEX | 41 | 43 | 41 |
| CALL | 28 | 27 | 27 |
| RETURN | 15 | 14 | 13 |

At the starting point, these paths load the active buffer pointers from the VM
2, 1, 2, 3, 1, 0, 1, 1 and 1 times respectively. The local-pointer paths load none
of them. Register allocation still spills some locals, so eliminating a pointer
load does not always shorten the handler. The old ADD path also calls a three
instruction `lz_value_int`, and LESS calls a four instruction `lz_value_bool`.
The new scalar stores remove those calls. Bool arithmetic and ordering still use
the slow semantic helpers, after the int and float fast paths have failed.

These counts explain the local change, but do not establish an end-to-end Ir
percentage. These audits use assembly counts. Do not run Valgrind on this phone, where its memory use caused a kernel panic. The target of returning
within 1% of stage 2b still needs the deterministic CI measurement.

### Building template strings

`BUILD_STRING` consumes a chunk-owned plan and its evaluated pieces. It counts
their bytes, allocates one result string, writes into that allocation and hashes
the completed bytes once. Statically written format specs are stored parsed in
the plan. `STRING_CHECK` validates a formatted piece at its original conversion
point and keeps its length on the value stack. A later expression cannot mask an
earlier format error. There is no separate heap string for a primitive conversion.

The compiler also keeps the original concatenation sequence. `STRING_BEGIN`
performs its first load and chooses the builder only with unlimited fuel and
heap, enough value-stack capacity, and primitive or string operands. The legacy
sequence ends with `STRING_FALLBACK`, which performs its original last ADD and
skips the builder. These instructions replace existing operations, so taking the
legacy branch adds no VM instructions. Finite budgets keep the old allocation
sequence, failure location and fuel accounting, including budgets set after
compilation. A small coroutine can take this branch without reserving more stack.

Only straight-line expressions made from loads and primitive operators are
eligible. Calls, temporary locals, branches, nested template builders, member
access, dynamic format specs and templates with more than 64 pieces retain the
original code. Units larger than 2 KiB also keep the original code, so duplicating
template paths does not exhaust the existing 16-bit branch range in large loops.
Units containing `finally` also retain the legacy code because the compiler can
copy those bodies many times even from a short source file. The spelling check
is conservative and can also disable the builder for a comment or literal.
Loading an object, array, dict, function or link also selects the
legacy branch before any conversion runs. This preserves custom text hooks and
their order relative to later effects. Ordinary string `+` chains are unchanged.
Pure constant expressions remain available to the existing constant folder.

Plans live beside the global-site cache in an uncharged chunk sidecar. This
preserves `sizeof(LzChunk)` and `sizeof(LzFunction)`, which matter to heap limits
and the object pool. Truncating bytecode drops plans that no longer fit and clears
cached global cells. Chunk destruction frees all plans and their piece arrays.

GE resolves both builder successors when translating. The JIT delegates the
conversion and final build semantics and hands the two control-flow boundaries
back to GE. Expected guard exits do not retire a hot JIT loop. Synthetic GE
opcodes start at 128, with static assertions separating them from bytecode.

On `bench/str_interp.lz` with `N = 1000`, the local allocation audit recorded
10,196 logical allocations before the builder and 2,196 afterwards. Both printed
`25335 1,000` and left zero live objects. Eight temporary strings per iteration
were removed. This is allocation evidence, not a claim about the final CI Ir.

### Calls and callbacks

Ordinary closures take a direct dispatch branch. When all positional parameter
slots are present and there is no rest parameter, argument arrangement returns
without building slots or entering the rearrangement helper's C frame. Defaults,
rest arguments and named arguments keep the shared slow machinery. Every frame
field is still initialized because resume, default prologues and error traces
read them. A normal return moves its result directly and only asks to close
upvalues when the current frame can own an open slot. Skink still runs after the
frame's values are dropped, preserving the collection and finalization order.

The C stack floor is reused within an execution activation while the callback
stays in the known thread-stack range. A new top-level activation resets it, a
different stack causes a fresh lookup, and coroutine resume restores the floor
measured for its caller. Platforms without known stack bounds keep the original
fallback measurement at each outer callback. The byte margin and depth limits
are unchanged.

Map and filter already use stack-local argument values without per-item argument
allocation. Their retained item copies are necessary because callbacks may remove
the source item or move its buffer. Sort keeps its stable bottom-up merge sort.
Its builtin comparison handles int/int and float/float directly and uses the
exact mixed-number comparison otherwise. Object comparison still works on an
owned copy and checks for mutation before replacing the array.

With the same local Clang AArch64 flags as the dispatch audit, an ordinary
two-argument closure call with available capacity took 132 instructions before
these changes and 81 afterwards. This includes the successful `arrange` and
`push_frame` helper bodies before the change, and excludes the Lizard function
body. The old arrangement helper alone took 40 instructions on that path and
used a 96-byte C frame. The common path now enters neither helper. These are
machine-code path counts, not an end-to-end benchmark claim.

### Dict key lookup

Strings already carry their FNV hash, computed when their final bytes are written.
They are immutable after publication, so dict lookup does not rescan string bytes
to hash them. This eager representation is retained, including a genuine zero
hash. Changing zero into a lazy sentinel would require changing interning, tables,
literal types and compiler constant keys together for no lookup benefit.

Each dict mixes that stored hash with its own seed. Insertion now shares the mixed
hash between the lookup and append, while copying recomputes it for the new dict's
seed. Int, bool and string hits compare their leaf values directly. String keys
check pointer identity, length and hash before comparing bytes. A hash collision
still requires equal bytes. Other probes keep the generic equality fallback, and
unsupported key errors still use the original diagnostic path. Float keys remain
invalid, and bool keys remain distinct from int keys.

Deletion still compacts the ordered entries and rebuilds the index. There are no
tombstones to invalidate. Tests cover a real equal-length FNV collision, a zero
hash, separately allocated equal strings, growth and repeated removal/reinsertion.
With the local AArch64 audit flags, a successful first-bucket int lookup takes
80 instructions instead of 127, including the old equality helpers. A hit using
the stored string pointer takes 72 instead of 123. These counts include hashing
and the index helper, without the surrounding VM opcode or caller.

### String methods

Case conversion checks for an entirely ASCII input before entering the Unicode
decoder. That path maps bytes directly into one newly allocated string and hashes
the finished bytes. Mixed text and invalid UTF-8 keep the original decoder and
simple Unicode mapping. Although string identity is value-based, unchanged text
still gets a new string to preserve allocation accounting and finite heap limits.
Fuel is charged before either path, using the original byte cost.

Byte searches use `memchr` for a one-byte needle. Linux builds use `memmem` for
longer needles, while other platforms retain the portable search. Defining
`LZ_NO_MEMMEM` exercises that fallback locally. Empty separators, trailing empty
pieces, embedded NUL and invalid UTF-8 keep their previous behavior. Split still
counts pieces first, allocates the same array capacity and charges the same fuel.

The writer already doubles its buffer capacity. That policy is retained, with a
direct store for a one-byte write. An ASCII case conversion now needs no temporary
writer buffer, removing one allocation/free pair for nonempty text up to 63
bytes, or more reallocations for longer text. Its one logical string allocation
is unchanged.
Generated case and byte-search corpora are also run against the saved pre-change
text implementation, including the exact output bytes and string hashes.
In a wrapper audit of 1,000 conversions of a 15-byte ASCII string, temporary
`realloc` calls fell from 1,000 to zero. Both paths made 1,000 logical string
allocations and left zero live heap bytes.

    lizard script.lz                   # Gecko Engine
    lizard --engine=ref script.lz      # the reference interpreter
    lizard --engine-stats script.lz    # what the engine did, on stderr


### Object construction and member sites

Instances already store their field slots and native payload in one allocation.
The class already resolves inherited fields into `field_refs` during layout.
That plan now also gives each field a small type tag, using existing padding in
`LzMember`. Primitive positive matches avoid the generic type matcher. An exact
class match also returns directly, while inheritance, literal types, unions and
other complex matches use the original rules. Arrays keep the original type
conformance and fuel charge. Every miss retains the original type diagnostic.
The slow checker stays out of line so its diagnostic buffers do not create a
large C frame on each primitive store.

Ref and GE share member-cache lookup, visibility checks and miss health. Ref
assigns at most 128 fixed member site numbers per chunk while compiling. A byte-offset
side table maps instruction starts to those numbers. Later sites use the ordinary
member operation without displacing earlier sites. The hot reference handler indexes
the cache directly, with no lazy allocation, modulo or collision clearing. Truncation
clears cached entries and removes mappings beyond the retained code. Site numbers
are not recycled during compilation. Chunk destruction frees both arrays. Global
slots are reserved alongside the constant table. These compiler-owned arrays remain
uncharged, so logical heap limits and object sizes do not change.

Only resolved members of the immutable class chain are cached. Getter calls,
callable fields, `init` and extension members keep the slow path. Link fields cache
only their slot. Every read dereferences its current cell through `lz_link_deref`,
including the nil result after a target dies. Link writes retain the general store
path. Scalar writes in ref can transfer the stack value directly into a cached slot
after a positive field type-tag check. Complex types and rejected values retain
the original checker and diagnostics.
The active frame's lexical visibility and write access are checked when filling
an entry. A receiver keeps the declaring class alive, and ids are never reused.
The existing extension count invalidates entries after registration. A compact
16-bit snapshot preserves the old GE instruction size. Counts above 65,535
always miss rather than wrapping into a false hit. As in GE's previous policy,
four hits make a useful run and eight short misses mark a site stuck. An already
cached class can still hit at a stuck site.

Class members cannot be replaced through `extend` in this language. Extensions
are searched after the class chain, and their results are never cached. A future
feature that permits member-array edits after instances exist must add class-chain
shape validation before reusing these borrowed member pointers.

Builtin method dispatch remembers the last receiver kind and its row in the immutable
method table. A hit selects the implementation without walking name groups or calling
`strcmp`. The two 16-bit cache fields use existing alignment padding in `LzNative`,
keeping that object at 80 bytes on the audited 64-bit target. Arity and unsupported-kind
errors still come from the dispatcher.

Ref and GE also recognize ordinary string, array and dict receivers at method lookup.
When the compiler already supplied a native fallback, they retain that fallback directly.
Otherwise a fixed site can remember the method table slot, guarded by its shape and the
receiver kind. Caching the slot preserves replacement of a method value without requiring
a shape change. Any registered extension disables these lookup shortcuts, since extension
lookup depends on lexical visibility. This is conservative and keeps extension methods
and their diagnostics on the original path.

Audited method groups that never call user code carry the leaf flag, including dictionary
`get`, string case conversion and array `push`. Existing CALL handlers then use the native
fast-call path. Groups that can invoke user code keep the general call path. Lookup and
CALL remain separate instructions so argument evaluation, fuel and error positions keep
their original order.

GE dispatches iterator length, iterator next, iterator consistency checks and the
keep-slot upvalue close directly to shared semantic helpers. Each handler charges one
instruction and synchronizes the VM state before the helper runs. The upvalue helper
still invokes the collector at the same point. Fused indexing also handles present integer
and string keys in dictionaries, using the dictionary's own hash lookup. Missing keys and
other kinds keep the ordinary diagnostic path. A fused result is retained before an
owned receiver can be released, and the existing fused fuel and stack guards still apply.

Unformatted template pieces have direct int, string, bool and nil writers. Float, debug and
formatted conversion retain their existing implementations. Format specs are already parsed
in compiler plans. GE calls the shared BUILD_STRING helper directly. Finite fuel and heap
limits retain the old guarded fallback and its allocation and error order.

Calls with missing optional positional arguments fill nil parameter slots inline, leaving
default evaluation in the callee prologue. Primitive parameter matches also run inline.
Bounds checks remain because reference chunks can be built without the compiler. Rest
arguments keep their ordinary allocation. Call arrangement, closure entry and the C-stack
guard are forced inline on supporting compilers, without changing stack-floor refresh rules.

Scalar sorting chooses a known-kind comparison after validation, starts with stable insertion
runs of 16 and merges those runs. Mixed numeric comparisons retain their exact int/float
rules. The original fuel formula and scratch allocation remain even when fewer passes run.

Fully supplied scalar construction without an init anywhere in the ancestry fills instance
slots directly and sets unset once. It skips the initial nil writes because no operation
between allocation and publication can fail or run user code. All other construction uses
the original path. No heap header or collector bookkeeping is removed.

Equality remembers a successful first miss for an immutable class without a custom operator.
The first miss still interns the operator name and preserves its heap-limit failure. Later
nil and identity comparisons can then bypass operator lookup. A class with custom equality
continues through the operator path, including for identical pointers and comparisons to nil.

The local layout audit found no changes to these sizes: `LzMember` 240 bytes,
`LzClass` 1,360, `LzInstance` 80, `LzChunk` 128 and `GeIns` 56. The accepted,
non-link int-field admission path takes 11 AArch64 instructions instead of 59,
including the old type-check callees and excluding the eventual slot write.
The old nested checkers needed another 400 bytes of C stack at their peak.
With caches disabled in an external probe, 1,000 get/store/method iterations
made 4,001 class lookups. Enabling the caches reduced that to five. Both probes
printed `999`. No profiling tool was used.


### GE delegation audit

A temporary opcode counter counted calls to `lz_vm_exec_one` on `dict_ops`,
`link_tree`, `obj_methods`, `obj_create`, `str_split_upper`, `mixed_wordfreq`,
`str_interp` and `array_hof`. Each size line was divided by 20, with a minimum
of one, and programs ran sequentially with `--engine=ge --jit=off`. This used
ordinary C counters, without Valgrind or benchmark CI.

The four most frequent delegated opcodes now have complete GE semantic paths:

| Opcode | Before | After |
| --- | ---: | ---: |
| `GET_FIELD` | 171,147 | 0 |
| `CHECK_PARAM` | 125,000 | 0 |
| `METHOD_LOOKUP` | 106,758 | 0 |
| `EQUAL` | 89,322 | 0 |

The total counted calls fell from 719,319 to 227,092, with identical checksums.
Guarded class and numeric shortcuts remain in place. Other values call the
shared VM field, parameter, method or equality semantics directly, after
charging one instruction and synchronizing the stack and instruction count.
Callbacks, type conformance and their extra fuel charges use the same code as
ref. GE still creates no independent diagnostic text. Cached link slots use the shared
link dereference implementation for each read.

Field-name decoding stays in the common field helper, while parameter operands
are already decoded in the IR. Keeping the JIT's delegate marks lets native
code keep its existing bailout behavior. The temporary counters are absent from
the runtime sources. Four small VM chunks assert both the result and zero
reference dispatches. Routing any of those paths back through the old dispatcher
makes that test fail even though the resulting language value is still correct.

### Array growth and leaf builtins

An array with no explicit heap limit doubles capacity below 1,024 items and grows
by half above that point. Doubling small buffers avoids frequent early copies;
the smaller steps for large buffers reduce unused space. A VM with a finite heap
limit keeps the previous doubling rule, so every capacity charge and the point at
which `--max-heap` refuses a push stay unchanged. Array storage still counts its
full capacity against the logical limit.

`lz_array_push` validates the type and cycle, then writes into the grown array
directly. It no longer calls the separate public append helper for that same
write. Arrays with no item type skip type matching for scalars, while still
running cycle checks for containers. A simple primitive tag check handles
matching scalar types. Compound types, array items and all cycle rules retain
their existing checks. `insert`, `remove`, `clear`, `copy` and `reverse` now use
the leaf-native route already used by `push` and `len`; it keeps their validation
and failure paths, while avoiding the general frame setup for a native call.

On the local AArch64 build, `lz_array_push` called `lz_array_check_store` and
then the separate `lz_array_append`. It now calls the checker once and writes
the retained value in its own path. The old `lz_array_push` had 43 instructions
and called both helpers; the final inline path has 96 instructions including
the successful store and one checker call. Capacity growth stays in a 51
instruction helper, reached only when the array is full. This counts static
instructions in the object file, not dynamic Ir or wall time.

For the existing large inputs, the item-buffer capacity estimate drops from
32 MiB to about 23.1 MiB for 1.2 million values, 16 MiB to about 15.4 MiB
for one million values, and 16 MiB to about 10.3 MiB for 600,000 values.
Those estimates cover the array item buffers, not whole-process peak memory.

### Catching failures in GE

GE translates `TRY_BEGIN` and `TRY_END`. A handler keeps the same frame index,
stack height and bytecode target as ref. On failure GE first publishes its stack
height and instruction count, then calls `lz_vm_unwind_to_handler`, the original
VM unwinder. Error construction, trace capture, upvalue closing and value release
therefore have one implementation. Fuel, heap and stack limits remain uncatchable.

After a catch, GE reloads the surviving frame's program and finds the handler's
IR position by binary search over the instruction offsets. Fusion keeps every
original instruction in the IR, including instructions inside fused sequences,
so the handler always has a resumable position. The search is on the error path
and requires no persistent byte-to-IR table. No pointer into the failed callee's
program is used after unwinding, since releasing that callee can free its cache.
Normal returns also discard handlers belonging to the departing frame.

Reference callbacks still unwind only frames above their own activation floor.
An error that escapes one returns through its native caller before GE can catch
it. Coroutines keep their reference loop and their own handler storage. Native
compilation is disabled for chunks containing either try opcode, since the JIT
does not model exceptional control-flow edges. Such chunks stay in GE, including
the catch body. Translated callees without handlers may still use the JIT.

## What GE is, and what it is not

GE started as an interpreter that does, once per run, the work the reference
interpreter repeats on every instruction: decode operands, resolve jumps, look up
globals, and run the most common instruction sequences as single instructions.
On top of that sits a **baseline JIT** (see [The JIT](#the-jit)) that compiles hot
loops and functions to x86-64 machine code. Both levels use the same IR, the same
exactness rules and the same test machinery, and a program the JIT cannot (or may
not) handle simply keeps running on the interpreter.

### Results

`bench/run.sh` runs every `bench/*.lz` on both engines. Best of 3 on one
2.1 GHz Xeon core, `-O3`, no LTO (your numbers will differ; the ratios are what
carry over). "GE" is the interpreter alone (`--jit=off`), "GE+JIT" the default:

| Benchmark | `ref` | GE | GE+JIT | JIT vs `ref` | JIT vs GE |
| --- | --- | --- | --- | --- | --- |
| `collatz` (globals, `n / 2`) | 4.21 s | 1.16 s | 0.41 s | 10.3x | 2.8x |
| `collatz_locals` (locals, `//`) | 3.71 s | 1.04 s | 0.11 s | 32.6x | 9.5x |
| `floats` (Leibniz series) | 0.63 s | 0.18 s | 0.04 s | 16.0x | 4.5x |
| `loops` (nested counting loops) | 1.21 s | 0.30 s | 0.05 s | 24.7x | 6.0x |
| `matrix` (60 products of 40x40) | 0.67 s | 0.17 s | 0.05 s | 13.8x | 3.4x |
| `prefix_sums` (array in place) | 0.37 s | 0.07 s | 0.04 s | 8.4x | 1.8x |
| `sieve` (1,000,000) | 0.58 s | 0.13 s | 0.07 s | 8.4x | 1.9x |
| `fib` (28 times `fib(25)`, ~7M calls) | 0.70 s | 0.23 s | 0.05 s | 13.4x | 4.6x |
| `calls` (defaults, typed args, closure) | 0.99 s | 0.47 s | 0.12 s | 7.6x | 3.9x |

`--engine-stats` shows where it comes from. On `collatz`, 515,655,227 bytecode
instructions run in 161,770,224 dispatches: every instruction is still
executed and counted, but 3.19x fewer trips through the dispatch loop are needed.

## How it works

### 1. Translation (`ge_translate.c`)

Before a chunk runs, its bytecode is translated into an array of `GeIns`, one
per bytecode instruction, in the same order:

* operands are decoded once (no byte reads, no operand range checks at run time);
* jump distances become instruction indexes;
* an instruction whose operands are not what the fast path assumes (a constant
  index out of range, a name that is not a string) is marked for the reference
  interpreter.

The mapping is **1:1**. That single design decision makes the rest simple: a
fused instruction replaces only the *first* slot of the sequence it covers. The
slots it covers keep their plain form, so the engine can always fall back to
plain execution at the same index, and a jump that lands in the middle of a
fused sequence just runs the plain instructions from there.

A chunk that cannot be translated faithfully (unknown opcode, an instruction cut
off by the end of the chunk, a jump into the middle of an instruction) is not
translated at all: it runs on the reference interpreter, which defines what
such bytecode does. The compiler never emits any of these.

### 2. Superinstructions

Sequences the compiler emits all the time are fused. `[load]` is any of
`GET_LOCAL`, `GET_GLOBAL`, `CONSTANT`; operands are read in place instead of
being pushed and popped.

| Pattern | Typical source | Dispatches |
| --- | --- | --- |
| `[load] [load] ARITH [SET_LOCAL \| SET_GLOBAL]` | `i = i + 1`, `a * b` | 3-4 -> 1 |
| `[load] ARITH [store]` (left operand on the stack) | `x * 2 + 1` | 2-3 -> 1 |
| `[load] [load] CMP JUMP_IF_FALSE POP` | `while i < n`, `if x > 0` | 5 -> 1 |
| `[load] CMP JUMP_IF_FALSE POP` | `n % 2 == 0` | 4 -> 1 |
| `[load] [load] INDEX` | `xs[i]` (the array is borrowed, no refcount traffic) | 3 -> 1 |
| `[load] INDEX` | `grid[i][j]` | 2 -> 1 |

`CMP JUMP_IF_FALSE POP` is fused only when the jump lands on a `POP`: then both
outcomes pop the condition and it never has to be materialized. (`a and b`
jumps with the value still on the stack and is left alone.)

### 3. Fast paths and delegation (`ge_exec.c`)

The executor handles the hot instructions itself: constants, locals, globals,
arithmetic and comparison on numbers, jumps, `NOT`, `NEGATE`, `INDEX`,
`SET_INDEX` (scalar over scalar), and native calls. Each fast path checks
*exactly* the types it was written for and changes nothing until every check
passed. On any doubt (a string operand, an overflow, a typed array, a full
stack, an index out of range, ...) it hands the instruction to the reference
interpreter through `lz_vm_exec_one`.

Two consequences:

* **GE never produces an error of its own.** Every error message, every error
  position comes from the reference code.
* **A new opcode works on GE the day it exists.** Anything GE does not know is
  delegated (`GE_OP_DELEGATE`), so `run.c` stays the one place where semantics
  live. Today `DEFINE_GLOBAL`, `CHECK_TYPE`, `ARRAY_NEW`, `ARRAY_PUSH` and
  `SLICE` are delegated on purpose: they are rare, or dominated by allocation.

### 4. Inline caches

* **Globals.** Each global access remembers the slot of the globals table where
  the name was last found. A hit costs one pointer comparison, no hashing. A
  stale slot (the table grew) fails the comparison and is refreshed.
* **Builtins.** `println`, `push`, `len`... are found by name every time they
  are called. Each call site caches the builtin, valid while no global has been
  added since (globals shadow builtins). This relies on tables never removing a
  key; a table that learns to delete needs an epoch counter at this one place
  (`builtin_cached` in `ge_exec.c`).

### 5. Call frames

GE handles `CALL` of a function, `RETURN` and `JUMP_IF_SUPPLIED` itself. Each
function is translated the first time it is called and the program is cached on
the function object, so it is freed with it. Running state (`code`, `constants`,
`base`) is swapped on every call and return; locals are `stack[base + slot]` in
plain and fused instructions alike.

* An exact positional call (as many arguments as parameters, no rest) pushes the
  frame inline. Anything else (defaults, rest, spread, named, arity errors) goes
  through the same `lz_vm_call` / `lz_vm_call_ex` that `ref` uses, then continues
  in GE.
* `RETURN` drops the frame inline when no variable of it was captured; otherwise
  it calls `lz_vm_return`, which closes the upvalues.
* `SET_UPVALUE` stores inline only what cannot close a reference cycle: a number,
  bool or nil, a string, any value into an upvalue that is already on one of Skink's
  watch lists (`LzUpvalue.watch.next != NULL`) or still open. A closure or array stored
  into a closed upvalue that is not watched yet goes through the slow path to
  `lz_vm_exec_one`, where `lz_skink_note_store` puts the upvalue on a watch list; every
  later store into it is inline again.
* A function whose code cannot be translated is run by `ref` for the whole call
  (`lz_vm_run_until`), and a failure inside it reports the offset `ref` found.
* Frame-changing instructions are never given to `lz_vm_exec_one`: they are done
  by GE itself.

## The contract: GE is indistinguishable from `ref`

For the same program, both engines produce the same:

* result and output (stdout and stderr);
* error message and error position;
* `vm->instructions` and the fuel left, including *where* metering stops;
* heap statistics: allocations, peak bytes, and nothing leaked.

Fuel is the delicate one. A fused instruction charges for all the bytecode
instructions it stands for, **and only if the remaining fuel covers all of
them**. Otherwise it runs plain, instruction by instruction, so `--fuel=N` stops
at exactly the same instruction as on `ref`.

## Testing

The contract is checked in layers (all registered with CTest):

1. **The existing suite, run through GE.** `test_run_ge`, `test_limits_ge`,
   `test_array_ge`, `test_compiler_ge`, `fuzz_run_ge`, `fuzz_compile_ge` are the
   original test sources compiled with `tests/engine_redirect.h`, which routes
   every `lz_vm_run` / `lz_vm_eval` call to GE. Same assertions, same
   expectations.
2. **`test_engine`**: about 100 programs (including functions: recursion, defaults, named, rest, spread, closures, errors) plus hand-built bytecode, each run on
   both engines and compared field by field (output, error, error offset,
   instructions, fuel, stack, heap statistics, leaks). It sweeps fuel from 1 to
   4000 on loops (metering must stop at the same instruction), heap limits, and
   shapes the compiler never emits: jumps into the middle of a fused run,
   missing locals, the value stack at its limit, shadowing a cached builtin,
   untranslatable chunks.
3. **`fuzz_engine`**: random Lizard programs (every operator, ints and floats,
   globals and locals, arrays of numbers and of objects) and random bytecode
   (balanced idioms with bounded loops, plus a wild mode with stack underflows
   and broken jumps), under random fuel. `fuzz_engine [programs [chunks [seed]]]`.
4. **Mutation checks.** While developing GE, bugs were injected on purpose (an
   off-by-one in a bound, a missing refcount, a dropped fuel charge, ...) to
   confirm the tests notice. Every injected bug that could change observable
   behaviour was caught; the few survivors turned out to be equivalent to the
   original.
5. **Sanitizers.** The whole set was run under AddressSanitizer and UBSan
   (`./configure --enable-sanitizers`), including hundreds of thousands of fuzz cases.

## Using it from C

    #include "lizard/engine.h"

    LzEngineStats stats;
    LzStatus status = lz_engine_run(LZ_ENGINE_GE, &vm, &chunk, &stats);

`lz_engine_eval` / `lz_engine_run` take an engine and an optional `LzEngineStats`.
`lz_ge_eval` / `lz_ge_run` have the same signatures as `lz_vm_eval` / `lz_vm_run`.

Two small additions to the core make this possible, and are the only changes
outside `src/engine/`: `lz_vm_exec_one` (run one instruction with the full
reference semantics, no accounting) and `lz_vm_eval_with` (`lz_vm_eval` with the
instruction loop supplied by the caller). Both are declared in `vm.h`.

## Adding or changing an opcode

1. Implement it in `run.c`. That is all that is *required*: GE delegates it.
2. To make it fast on GE, add a case in `ge_exec.c` (and remove it from the
   delegate list in `ge_translate.c`). The rules: check exactly what you
   handle, change nothing before every check has passed, and `goto run_plain`
   (after the fuel tick) for everything else.
3. Add a program using it to the corpus in `tests/test_engine.c`, and teach the
   generators in `tests/fuzz_engine.c` about it.

## The JIT

`ge_jit_x64.c` and `ge_jit_a64.c` (with the assemblers in `ge_x64.h` and
`ge_a64.h`) compile a translated program to machine code. The text below is
written for x86-64 (register names); the AArch64 backend is the same design,
see "AArch64" at the end of this section. It is a *baseline* JIT: no type feedback, no register
allocation across instructions, no inlining. What it removes is dispatch,
operand decoding and most memory traffic for the instruction sequences GE
already fuses, which is where an interpreter spends its time.

### When it runs

* Platform: x86-64 (`ge_jit_x64.c`, System V ABI) and AArch64 (`ge_jit_a64.c`:
  Linux, Android/Termux, macOS), both through the same stack analysis
  (`ge_jit_common.h`) and runtime helpers (`ge_jit_rt.c`). Elsewhere, or with
  `./configure --disable-jit`, the same sources build a stub and everything runs on
  the interpreter. If the system refuses executable memory (some hardened
  Android configurations), the JIT switches itself off and the interpreter runs.
* Policy: the interpreter counts calls and loop back-edges per program; at 50 it
  compiles the program and enters the native code at the back-edge or call. Code
  that keeps handing instructions back to the interpreter (more than 256 bail-outs
  and more than half as many as entries) is retired and left to the interpreter.
* `--jit=off|on|eager` (or `LZ_JIT=...`, or `lz_engine_set_jit`). `eager`
  compiles every program on first sight; the test suite uses it to run
  everything native.
* **Never with a fuel limit.** Native code does not meter fuel, so a run with
  `--fuel` (fuel is not `LZ_FUEL_UNLIMITED`) always uses the interpreter, whose
  accounting is exact. `vm->instructions` is still exact under the JIT.

### Design

Native code works on the VM's own value stack and frame stack, so the state is
always in the interpreter's form and control can pass either way at an
instruction boundary:

* The JIT computes, per instruction, how many values the frame has on the stack
  (a stack-height analysis). Locals and temporaries are then `[r14 + 16 * slot]`
  operands. A chunk whose heights are inconsistent (hand-made bytecode that
  underflows the stack) is not compiled.
* Fused instructions (`BIN`, `CMPJ`, `INDEX`, ...) become one block: read operands
  in place, check types, compute, store. Int and float paths are both inline (the
  float path in a cold section). Overflow, division by zero, out of range indexes,
  strings and everything else unusual **bail out**: all checks of an instruction
  pass before anything is changed, so the interpreter can run that very
  instruction and produce the exact result or error.
* Calls to closures with exactly the declared parameters push the frame inline and
  use a native `call`; `RETURN` pops it and `ret`s. Recursion stays in machine
  code. Natives, defaults, rest parameters and errors go through
  `lz_vm_call`; instructions without an inline form (`DEFINE_GLOBAL`, `CLOSURE`,
  `ARRAY_*`, `SLICE`, ...) run the reference semantics through `lz_vm_exec_one`.
  `CALL_EX` bails out to the interpreter.
* Instruction counts are exact: a block adds its length to `r15` where it ends or
  calls out, and a bail-out stub compensates for what had not run yet.
* Machine code lives in `mmap`ed pages written first and then made executable
  (never writable and executable at once), one mapping per compiled program,
  freed with the program.

Debugging aids: `LZ_JIT_DUMP=prefix` writes each compiled program to
`prefix.N.bin` (`objdump -D -b binary -mi386:x86-64 -M intel`).

### Testing

* `test_jit` runs ~65 programs (overflow in the middle of a loop, float modulo,
  negative indexes, deep recursion and stack overflow, closures, errors inside
  natively called functions, defaults/rest/named arguments, typed parameters,
  heap limits, ...) on `ref` and on GE with the JIT eager and with the normal
  policy, and demands identical results; it also checks that they really ran
  native and that fuel limits keep them off it.
* `test_engine`, `fuzz_engine` and the `*_ge` conformance variants run a second
  time with `LZ_JIT=eager` (the `*_jit` tests in CTest).
* The whole suite passes under ASan and UBSan (x86-64), with `./configure --disable-jit`, and
  under qemu-user on AArch64.

### AArch64

Same structure, different instruction set. Differences worth knowing:

* Registers: x19 the VM, x20 the exit block, x21/x22 the frame's base index and
  address, x23 the instruction counter, x24/x25 anchors on `vm->stack_count` and
  `vm->frame_count` (the fields behind the 256 KiB value stack are too far for a
  scaled load offset from x19). x0-x15 are scratch, x16 holds call targets and
  x17 is the address scratch of the load/store helpers; x18 is never touched
  (the platform reserves it).
* A native function keeps 16 bytes at `sp` (saved `lr` and a scratch slot), so
  `sp` is always 16-byte aligned. The instruction cache is flushed
  (`__builtin___clear_cache`) before the pages become executable.
* Overflow checks use the V flag (`adds`/`subs`/`negs`) and `smulh` for
  multiplication; float comparisons need no parity handling, since one condition
  code per comparison is false when unordered.
* Conditional branches reach +-1 MiB, so a program whose code would exceed
  1 MB is not compiled (it runs on the interpreter).
* Tested by running the whole suite (including `test_jit` and the eager variants)
  under `qemu-aarch64` on a cross-built binary. That checks correctness, not
  speed: qemu's timings say nothing about a real core.

### Limits

* Two backends: x86-64 and AArch64.
* `CALL_EX`, typed-array element stores, `NOT` of an object and float
  floor-division bail out (correct, but at interpreter speed).
* Not compatible with CPU shadow-stack enforcement (CET), because exits unwind
  the native stack without `ret`.
* Values live in memory between instructions. Keeping them in registers (and
  specializing on types) is the next step.

## Known gaps

* `ARRAY_NEW`, `ARRAY_PUSH`, `SLICE` and `DEFINE_GLOBAL` are
  delegated. Building a large array with `push` goes through the native-call fast
  path, but each `push` is still a call.
* Translation runs on every `lz_ge_eval`. For a host that runs the same chunk
  many times, a cached `GeProgram` would avoid it.
* `CALL_EX` (spread and named arguments), calls that need defaults or a rest
  array, and arity errors take the slower path through `lz_vm_call`/`lz_vm_call_ex`
  (shared with `ref`); only exact positional calls are inlined.
* GE needs no compiler extensions: it builds with `-std=c11 -Wpedantic`. Computed
  `goto` dispatch might add some speed on GCC and Clang, at the price of that.

Imports have a direct GE handler. It checks whether the module has finished running and returns at
that site on later executions. A first execution runs the module and copies its exports. The JIT
delegates this single instruction through the regular interpreter helper, so functions containing
imports remain translatable.

## Roadmap

The baseline JIT is done (above). What remains, in order:

1. **Type feedback and register allocation.** Record the types each site sees
   and keep values in registers inside a loop; an `ADD` that has only seen small
   ints becomes an int add behind a guard.
2. **Bail-outs that resume.** Today a bail-out re-enters native code at the next
   leader; resuming in the middle of a block would make guard failures cheaper.
3. **More inline instructions** (native calls like `push` and `len`, `CALL_EX`).
4. **Other targets** (RISC-V, Windows on either architecture).
5. **Only then**, a tracing JIT in the LuaJIT style.

Every stage can be tested the way GE and the JIT are: run it against `ref` on
the same programs and demand identical behaviour.
