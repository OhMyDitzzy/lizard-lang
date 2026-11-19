# Developing

## Editor setup

`.clangd` reads `compile_commands.json`. Autotools does not write one; generate it with
[bear](https://github.com/rizsotto/Bear) (`./configure --with-build-type=debug && bear -- make`) or
with `compiledb make`.

`.clang-tidy` and `.clang-format` are at the repository root. See
[Architecture](ARCHITECTURE.md) for the layout, the memory model, and how to add a
builtin.

## Running the tests

Build with tests on, then run the suite:

    ./autogen.sh                      # git checkout only
    ./configure && make -j
    make check -j                     # one test: make check TESTS=test_links; results: test-suite.log

Without Autotools: `sh tools/build-plain.sh check` (see [building/plain.md](building/plain.md)).

No test runs the examples in `docs/`. The author writes each example and runs it by hand,
so check it again whenever the behavior it shows changes.

## Benchmarks

`bench/` holds the benchmark programs (`bench/*.lz`) and the scripts that measure them. Every
program is deterministic, prints a small checksum, and has one size line `let N = ...` that the
scripts can divide. The measuring runs on GitHub Actions, on one runner, so that a change and
its base are compared on the same machine.

To run it, open the Actions tab, pick the `bench` workflow and press "Run workflow" on the
branch to measure. The inputs:

| Input | Meaning |
| --- | --- |
| `base` | the ref to compare with, `main` by default; empty measures only this ref |
| `runs` | timed runs per benchmark and engine, 7 by default |
| `engines` | `ref`, `ge` (the interpreter, `--jit=off`) and `jit` (`--jit=on`) |
| `profile` | also count instructions with callgrind, on by default |
| `scale` | divides every size, 1 is the full size |

The job builds both refs in release mode, runs every benchmark once at a tiny size as a smoke
test (it fails when an engine prints something different), takes turns timing the base and the
head one benchmark at a time, and then counts instructions. Numbers never fail the run, only a
failed build, a failed smoke test or `OUTPUT DIFFERS` does.

The summary of the run shows the whole report. Two artifacts are kept for 30 days:
`bench-report` is a single file, `bench-report.md`, that stands alone: metadata, the smoke
results, then tables of time, instructions and peak memory, the top functions of every
benchmark and the warnings. `bench-data` has the TSV files behind it and the `profile/` folders.

There are two kinds of numbers because they fail in different ways. Wall time is what users
feel, but on a shared runner it is noisy, so `bench/compare.sh` marks a change smaller than
twice the combined deviation as `noise`. Instructions (`Ir`) come from callgrind at a smaller
size and are the same on every run, so a change in `Ir` is a real change. Use time to see what
matters and `Ir` to decide. The JIT is not profiled: callgrind cannot name the machine code it
generates.

The scripts also run by hand:

    bench/measure.sh ./lizard times.tsv            # RUNS, ENGINES, SCALE and ONLY change it
    bench/compare.sh before.tsv after.tsv          # a Markdown table, time or instructions
    bench/profile.sh ./lizard profile-out          # needs valgrind

The rule is that nothing is optimized without numbers: a change that claims to make something
faster comes with a `bench-report` that shows it, and one that shows only `noise` in time and no
change in `Ir` is not an optimization.

## Stage 3 checkpoint

Do not run Valgrind, Callgrind or DHAT on this phone. The user prohibited them
because their memory use caused a kernel panic. Installation does not revoke
that restriction. Use assembly counts or bounded allocation probes instead,
and run builds and test configurations sequentially with `--jobs=1`.

The current worktree implements parts 0 through 9 of the stage 3 optimization
work. Parts 0 through 10 are complete. The GE try handlers share the reference
unwinder and resume through the translated handler block; chunks with try
opcodes stay out of native JIT code. Delegation counters are kept out of the
runtime sources.

The completed work has regression coverage in `truth_table.lz`,
`hot_contexts.lz`, `test_memory.c`, `test_strings.c`, `test_chunk.c`,
`test_higher.c`, `test_classes.c`, `test_dict.c`, `test_methods.c` and
`test_jit.c` and `test_ic.c`. The normal test list already runs these on ref, GE and eager JIT
where applicable. Local checks also use
pool-disabled and poison builds. ASan cannot run on this Termux host, so its
pool-disable detection was checked by preprocessing and compilation. Targeted
UBSan runs cover the allocator, string builder, call helpers, dict, text methods
and buffer writer. Mutation checks cover the new guards and fast paths. The
template compiler also has large-loop and copied-finally jump-range regressions.

At this checkpoint, all 117 tests pass in each of the normal, no-pool, poison
and no-pool-plus-poison builds. Differential checks compare 4,844 runs against
saved binaries across engine modes, fuel limits and heap limits. Another 96
checks compare error text and heap statistics. All 91 example, module and small
benchmark programs match across the four engine/JIT choices, for 364 comparisons.
The GE try/catch tests also compare fuel and heap boundaries directly, run the
full existing try suite on GE with eager JIT, and mutate unwinding and handler
target paths to ensure the regression tests detect them.

The dispatch, template allocation, call and dict instruction audits are in
[ENGINE.md](ENGINE.md). Pool allocation evidence and exact-size accounting are in
[MEMORY.md](MEMORY.md). The stage 2b Ir target for ref still needs the user's
deterministic CI measurement. Local checks run small benchmark inputs only to
compare outputs and allocation counts, without running the benchmark CI.

## Sanitizers

The stage 4 implementation, guarded paths, delegated-opcode measurements, memory audit and
explicit deferrals are recorded in [PERFORMANCE-STAGE4.md](PERFORMANCE-STAGE4.md). Its final
verification section describes the current checkpoint rather than a timing or Ir claim.

    mkdir build-san && cd build-san
    ../configure --enable-sanitizers --with-build-type=debug
    make -j && make check

AddressSanitizer, UBSan, and LSan run under `.github/workflows/sanitizers.yml` on
every push and pull request, and on a weekly schedule.

## Poison mode

Poison mode fills freed blocks with 0xDD and keeps them in a quarantine, catching
use-after-free without ASan:

    mkdir build-poison && cd build-poison
    ../configure --enable-poison --with-build-type=debug
    make -j && make check
    cd .. && sh tests/poison_selftest.sh

`tests/poison_selftest.sh` puts an old link bug back (`-DLZ_TEST_REINTRODUCE_LINK_BUG1`)
and expects `test_links` to fail, proving the test catches the bug.

## Fuzzing

The fuzzers are built as test binaries and can be run directly:

    ./fuzz_links
    ./fuzz_compile
    ./fuzz_run
    ./fuzz_engine

`tests/fuzz_links.c` generates programs, predicts every `.alive` with a small model,
and checks `lz_link_check` and `live_objects == 0`.

## Adding a documented page

Code blocks in `docs/` are not run by any test, so an example must produce the output shown
beside it because the author ran it. To add a page:

1. Create `docs/NAME.md` written in the natural style used by the other pages — an
   opening that explains what the feature is for, examples that build on each other,
   and errors explained where they naturally arise.
2. Add it to [the index](README.md) and, when it is a user-facing page, to `README.md`.
3. Run every example by hand with `lizard` and fix any output that does not match.

The engine and sanitizer behaviour is part of what the tests check, so keep examples
engine-agnostic unless the page is specifically about engines.

Internal layout and how to add a builtin are in [Architecture](ARCHITECTURE.md).
Ownership, links, and the skink collector are in [Memory](MEMORY.md). The Gecko
Engine and the JIT are in [Engine](ENGINE.md).
