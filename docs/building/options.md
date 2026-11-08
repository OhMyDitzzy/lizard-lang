# Options

`./configure --help` lists them too.

| Option | Meaning |
| --- | --- |
| `--enable-jit[=auto\|yes\|no]` | The JIT of the Gecko Engine. Default `auto`: on for `x86_64` and `aarch64`/`arm64` on Linux and macOS; elsewhere the build gets `-DLZ_NO_JIT` and says so |
| `--disable-jit` | no JIT: everything runs on the GE interpreter |
| `--enable-lto` | `-flto`, when the compiler supports it (a warning when not) |
| `--enable-sanitizers` | `-fsanitize=address,undefined -fno-omit-frame-pointer`; an error with `--enable-static-binary` |
| `--enable-poison` | `-DLZ_POISON`: freed objects are poisoned and quarantined (see [MEMORY.md](../MEMORY.md)) |
| `--enable-static-binary` | link `lizard` with `-static` (not `--enable-static`, which means libtool libraries elsewhere) |
| `--enable-werror` | `-Werror` |
| `--disable-tests` | do not build the tests; `make check` only says so |
| `--enable-official`, `--with-signing-key=FPR` | the `official` channel; see [release.md](release.md) |
| `--with-build-type=T` | `release` (default, `-O2 -DNDEBUG`), `debug` (`-g -O0`), `relwithdebinfo`, `minsizerel`. Ignored when you give `CFLAGS` |
| `--prefix=DIR`, `--host=TRIPLET` | the usual GNU ones |
| `CC`, `CFLAGS`, `LDFLAGS`, `CPPFLAGS` | compiler and flags; `CFLAGS` is respected as given |

Warnings (`-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wstrict-prototypes -Wmissing-prototypes
-Wformat=2`) are tried one by one and only the supported ones are used. Note that `--enable-werror` currently
stops at warnings that exist in the sources (for example `-Wshadow` in `src/core/run.c`).

## Examples

```
./configure --with-build-type=debug                  # debugging
./configure --enable-sanitizers --with-build-type=debug
./configure --enable-poison --with-build-type=debug   # then: make check
./configure --enable-lto
./configure --enable-static-binary                    # musl or glibc static executable
./configure --disable-jit
./configure CC=clang CFLAGS="-O1 -g"
```
