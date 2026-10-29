# Building and releasing

## Quick start

```
./build.sh detect                 # what this machine is, and what it can build for
./build.sh check                  # build, then run the tests
./build.sh package                # dist/lizard-<version>-<target>.tar.gz + SHA256SUMS
```

`build.sh` is a driver around CMake (so plain `cmake -S . -B build` keeps working). It needs
bash 3.2+, cmake 3.16+, a C11 compiler, and for packages tar plus gzip or xz. git is optional
and gpg is needed only for official builds.

## What `lizard --version` says

```
0.1.0-dev.12+gabc1234.dirty.1a2b3c4d
build    dirty (Release)
commit   abc1234def... 2026-10-29T08:31:00+07:00
channel  local (unsigned)
target   x86_64-linux-gnu, GNU 13.3.0
features jit=on lto=off sanitizers=off static=on
```

The first line is the version alone. The same facts are in `share/doc/lizard/BUILDINFO` of an
install or package (`key=value`), and in `lizard/version.h` (`LZ_BUILD_KIND`, `LZ_BUILD_COMMIT`, ...).

### Kind of build

| Kind | When | Version |
| --- | --- | --- |
| `release` | clean tree, HEAD is exactly the tag `vX.Y.Z` | `0.1.0` |
| `snapshot` | clean tree, N commits past the last tag (or no tag) | `0.1.0-dev.N+g<sha>` |
| `dirty` | uncommitted changes, including untracked files | `0.1.0-dev.N+g<sha>.dirty.<hash>` |
| `source` | no git: a source tarball | `0.1.0+g<sha>` from its `.buildinfo`, else `0.1.0+source` |

`<hash>` is eight hex digits of a SHA-256 over the uncommitted diff and the untracked files, so two
dirty builds of different edits never carry the same version. Build directories inside the source
tree are not counted as changes. The information is regenerated on every build, not only when
CMake is configured, so the commit and the dirty state are never stale; nothing is recompiled
unless they changed. The date is the commit date (or `SOURCE_DATE_EPOCH`), never the wall clock, so
builds are reproducible.

The CMake build type (`Release`, `Debug`, ...) is separate and shown in parentheses.

### Channel: local or official

* **local** (default): an unsigned build for your own use.
* **official**: a build meant to be signed by the maintainers. It requires a clean tree with a
  known commit (CMake refuses a dirty one, at configure time and at build time) and a signing key.
  `build.sh package --official` signs every tarball and `SHA256SUMS` with a detached GPG signature
  (`*.asc`). The binary then reports `official (signed, key <fingerprint>; verify the .asc of the
  package)`.

The binary cannot vouch for itself: that line is a claim, and the signature of the package is the
proof. Check it with `./build.sh verify` (see below).

## build.sh

```
./build.sh [COMMAND] [OPTIONS] [VAR=value]
```

| Command | What it does |
| --- | --- |
| `detect [--porcelain]` | OS, architecture, ABI, C library, compiler, tools, git state, and which cross targets have a toolchain |
| `configure`, `build`, `check` | CMake configure; configure and build; build and run the tests |
| `package` | build, install into a staging tree, write `dist/lizard-<version>-<target>.tar.gz` and `SHA256SUMS` |
| `verify [dir]` | check `SHA256SUMS` and every `.asc` in `dist/` |
| `sourcetar` | `dist/lizard-<version>-src.tar.gz` from the committed tree, with a `.buildinfo` |
| `clean` | remove `build/` |

Common options (`./build.sh --help` has all of them):

| Option | Meaning |
| --- | --- |
| `--targets=LIST`, `--host=T` | `native` (default), `all`, or names like `aarch64-linux-musl` |
| `--build-type=T`, `--debug` | `Release` (default), `Debug`, `RelWithDebInfo`, `MinSizeRel` |
| `--enable-jit`/`--disable-jit` | default: on where a backend exists |
| `--enable-static`, `--enable-lto`, `--enable-sanitizers`, `--enable-werror`, `--disable-tests` | as named |
| `--official`, `--sign-key=KEY` | signed build; key defaults to `$LIZARD_SIGNING_KEY`, else your first GPG secret key |
| `--compress=gz\|xz`, `--outdir=DIR`, `--builddir=DIR` | package format and locations |
| `--check` | with `package`: run the tests first (native target only) |
| `--strict`, `--keep-going` | a missing toolchain is an error; continue after a failed target |
| `CC=... CFLAGS=... LDFLAGS=...` | compiler and flags for this machine's target |

### Platforms, ABIs and tarballs

A target is `<arch>-<os>[-<abi>]`: `x86_64-linux-gnu`, `aarch64-linux-musl`, `armv7-linux-gnueabihf`,
`riscv64-linux-gnu`, `aarch64-darwin`, ... GNU triplets (`x86_64-pc-linux-gnu`, `arm64-apple-darwin`)
are accepted and normalized. `--targets=all` is the Linux matrix (gnu and musl on x86-64 and
AArch64, armv7 hard-float, riscv64).

For a target other than the machine's own, `build.sh` looks for `<triplet>-gcc`, then `zig cc`,
then `clang --target=... --sysroot=...` (give `--sysroot=`); with a single target, `CC=` overrides.
A target with no toolchain is skipped with a warning (an error with `--strict`). On macOS, `x86_64` and
`aarch64` build with the same compiler (`-arch`). Cross-built targets are not tested: the test suite runs
only for the machine's own target. The JIT exists for x86-64 and AArch64 on Linux and macOS; elsewhere
the same binary runs everything on the GE interpreter. Windows (`x86_64-windows-mingw`) is accepted but
untested.

A tarball holds `lizard-<version>-<target>/` with `bin/lizard`, `BUILDINFO`, `share/doc/lizard/` and
`share/lizard/examples/`. It is relocatable. With GNU tar the archive is deterministic
(sorted entries, fixed owner and mtime, `gzip -n`): building the same commit twice gives
byte-identical files.

### A signed release

```
git tag v0.2.0 && git push --tags
./build.sh package --official --targets=all --sign-key=<KEYID>
./build.sh verify --expect-key=<FINGERPRINT>     # what a user does, with your public key imported
```

`--official` refuses a dirty tree, and warns when HEAD is not a `vX.Y.Z` tag (the result is then a
signed snapshot). gpg asks for your passphrase through its agent.

### Source tarballs

`./build.sh sourcetar` archives the committed tree plus a `.buildinfo` recording the commit and kind.
A build from that tarball (no git) reports the same version and commit as the repository it came from.

## Platform notes

* musl: `--enable-static` gives a self-contained binary. macOS has no static executables, so the option
  is ignored there.
* `--enable-sanitizers` cannot be combined with `--enable-static`.
* `check` skips `skink_cli` on a platform without a JIT, because that test fails on purpose when the
  JIT did not run.
