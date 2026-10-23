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
| `matrix` (150x150 multiply) | 0.67 s | 0.17 s | 0.05 s | 13.8x | 3.4x |
| `prefix_sums` (array in place) | 0.37 s | 0.07 s | 0.04 s | 8.4x | 1.8x |
| `sieve` (1,000,000) | 0.58 s | 0.13 s | 0.07 s | 8.4x | 1.9x |
| `fib` (`fib(32)`, ~7M calls) | 0.70 s | 0.23 s | 0.05 s | 13.4x | 4.6x |
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
  `-DLZ_ENABLE_JIT=OFF`, the same sources build a stub and everything runs on
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
* The whole suite passes under ASan and UBSan (x86-64), with `-DLZ_ENABLE_JIT=OFF`, and
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
