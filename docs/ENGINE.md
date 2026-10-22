# Engines

Lizard runs bytecode on one of two engines. They implement exactly the same
language and differ only in speed.

| Engine | Where | What it is |
| --- | --- | --- |
| `ref` | `src/core/run.c` | The reference interpreter: one `switch`, no tricks. It *defines* what every instruction does. |
| `ge` (default) | `src/engine/` | The Gecko Engine: a faster interpreter for the same bytecode. |

    lizard script.lz                   # Gecko Engine
    lizard --engine=ref script.lz      # the reference interpreter
    lizard --engine-stats script.lz    # what the engine did, on stderr

## What GE is, and what it is not

GE is **not a JIT**. It never generates machine code. It is an interpreter that
does, once per run, the work the reference interpreter repeats on every
instruction: decode operands, resolve jumps, look up globals. It also runs the
most common instruction sequences as single instructions.

A JIT in the style of LuaJIT is the next level (see [Roadmap](#roadmap-toward-a-jit)).
GE is the foundation it would stand on: the same IR, the same exactness rules,
and the same test machinery.

### Results

`bench/run.sh` runs every `bench/*.lz` on both engines. Best of 3 on one
x86-64 core, `-O3`, no LTO (your numbers will differ; the ratios are what carry over):

| Benchmark | `ref` | `ge` | Speedup |
| --- | --- | --- | --- |
| `collatz` (globals, `n / 2`) | 3.43 s | 1.03 s | 3.3x |
| `collatz_locals` (locals, `//`) | 3.18 s | 0.88 s | 3.6x |
| `floats` (Leibniz series) | 0.54 s | 0.16 s | 3.2x |
| `loops` (nested counting loops) | 0.95 s | 0.27 s | 3.4x |
| `matrix` (150x150 multiply) | 0.57 s | 0.15 s | 3.8x |
| `prefix_sums` (array in place) | 0.30 s | 0.07 s | 4.3x |
| `sieve` (1,000,000) | 0.44 s | 0.13 s | 3.4x |
| `fib` (`fib(32)`, ~7M calls) | 0.76 s | 0.25 s | 3.0x |
| `calls` (defaults, typed args, closure) | 1.03 s | 0.53 s | 1.9x |

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
   (`-DLZ_ENABLE_SANITIZERS=ON`), including hundreds of thousands of fuzz cases.

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

## Known gaps

* `ARRAY_NEW`, `ARRAY_PUSH`, `SLICE`, `DEFINE_GLOBAL` and `CHECK_TYPE` are
  delegated. Building a large array with `push` goes through the native-call fast
  path, but each `push` is still a call.
* Translation runs on every `lz_ge_eval`. For a host that runs the same chunk
  many times, a cached `GeProgram` would avoid it.
* `CALL_EX` (spread and named arguments), calls that need defaults or a rest
  array, and arity errors take the slower path through `lz_vm_call`/`lz_vm_call_ex`
  (shared with `ref`); only exact positional calls are inlined.
* GE needs no compiler extensions: it builds with `-std=c11 -Wpedantic`. Computed
  `goto` dispatch might add some speed on GCC and Clang, at the price of that.

## Roadmap toward a JIT

GE removes interpretive overhead; it does not remove interpretation. Native code
is the next step, and the order matters:

1. **Type feedback.** Record the types each site sees (GE already has per-site
   state) and specialize: an `ADD` that has only ever seen small ints becomes an
   int add behind a one-instruction guard.
2. **A register IR.** Turn the stack bytecode into three-address code over stack
   slots. This is also what a JIT wants as input, and it makes common
   subexpressions and dead stores visible.
3. **A baseline JIT.** Emit machine code from the GE IR for hot loops only
   (counters on back-edges). Needs one backend per architecture: x86-64 and
   ARM64 are different programs, and executable memory is restricted on some
   platforms.
4. **Only then**, a tracing JIT in the LuaJIT style, with deoptimization back to
   GE using exactly the delegation mechanism above.

Every stage can be tested the way GE is: run it against `ref` on the same
programs and demand identical behaviour.
