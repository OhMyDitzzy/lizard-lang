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

The `docs_check` test runs every code example in `docs/` and fails when an example's
output does not match, so documentation examples are executed by the test suite.

## Sanitizers

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

Every code block in `docs/` is run by `docs_check` (listed in `tests/tests.list`), so an
example in the docs must produce the output shown beside it. To add a page:

1. Create `docs/NAME.md` written in the natural style used by the other pages — an
   opening that explains what the feature is for, examples that build on each other,
   and errors explained where they naturally arise.
2. Add it to [the index](README.md) and, when it is a user-facing page, to `README.md`.
3. Run `make check TESTS=docs_check` and fix any mismatched example.

The engine and sanitizer behaviour is part of what the tests check, so keep examples
engine-agnostic unless the page is specifically about engines.

Internal layout and how to add a builtin are in [Architecture](ARCHITECTURE.md).
Ownership, links, and the skink collector are in [Memory](MEMORY.md). The Gecko
Engine and the JIT are in [Engine](ENGINE.md).
