# Building without Autotools: `tools/build-plain.sh`

One POSIX shell script. It needs only `sh`, `awk`, a C compiler (`$CC`, default `cc`) and the compiler's
`ar`. No `make`, `cmake` or `autoconf`. It reads the same `mk/sources.am` and `tests/tests.list` as the
Autotools build, and runs the same `tools/buildinfo.sh`, so the binary and `lizard --version` are the same.
The `sources_sync` test keeps the two paths from drifting apart.

## Usage

```
sh tools/build-plain.sh [build|check|install|clean] [--builddir=DIR] [--jobs=N] [--prefix=DIR]
                        [--debug] [--no-jit] [--lto] [--sanitizers] [--poison] [--static] [--werror]
                        [--official --signing-key=FPR] [CC=clang] [CFLAGS=...] [LDFLAGS=...]
```

| Command | Does |
| --- | --- |
| `build` (default) | compiles into `build-plain/` (or `--builddir`) and links `build-plain/lizard` |
| `check` | builds, then runs every test of `tests/tests.list` in parallel; a summary, exit status 1 on a failure. Logs in `build-plain/test-logs/` |
| `install` | `--prefix=DIR` (default `/usr/local`), same layout as `make install`; honours `DESTDIR` |
| `clean` | removes the build directory |

The options mean what the `configure` options of [options.md](options.md) mean: `--debug` is
`--with-build-type=debug`, `--static` is `--enable-static-binary`, and so on. The JIT is chosen from `uname -m` and
`uname -s`; `--sanitizers` with `--static` is an error. Compiling is parallel (`--jobs`, default the CPU
count) and incremental: a file is rebuilt when its `.c` or any header is newer than its object. The build directory
may be outside the source tree.

```
sh tools/build-plain.sh check --jobs=4
sh tools/build-plain.sh install --prefix=$HOME/.local
CC=clang sh tools/build-plain.sh build --debug
```

## Termux and Alpine

```
pkg install clang          # Termux
apk add build-base         # Alpine
sh tools/build-plain.sh check
```

## When to choose it

Pick it when you cannot or do not want to install Autotools or make, on a small or unusual machine, or for a quick
check. Pick Autotools for packaging, `make dist`, `DESTDIR` staging, cross builds and `make check` selection
(`TESTS=...`). The plain script does not cross compile.
