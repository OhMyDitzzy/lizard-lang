# Releasing and build information

## What `lizard --version` says

```
0.1.0-dev.12+gabc1234.dirty.1a2b3c4d
build    dirty (Release)
commit   abc1234def... 2026-10-29T08:31:00+07:00
channel  local (unsigned)
target   x86_64-linux-gnu, GNU 13.3.0
features jit=on lto=off sanitizers=off static=on
```

The first line is the version alone. The same facts are in `share/doc/lizard/BUILDINFO` (`key=value`) and in
`lizard/version.h`. They are written by `tools/buildinfo.sh` at `configure` and again on every `make` (the files are
only rewritten when their content changes, so nothing is rebuilt needlessly).

### Kind of build

| Kind | When | Version |
| --- | --- | --- |
| `release` | clean tree, HEAD is exactly the tag `vX.Y.Z` | `0.1.0` |
| `snapshot` | clean tree, N commits past the last tag (or no tag) | `0.1.0-dev.N+g<sha>` |
| `dirty` | uncommitted changes, including untracked files | `0.1.0-dev.N+g<sha>.dirty.<hash>` |
| `source` | no git: a source tarball | `0.1.0+g<sha>` from its `.buildinfo`, else `0.1.0+source` |

`<hash>` is eight hex digits of a SHA-256 over the uncommitted diff and the untracked files. Build directories
inside the source tree are not counted. The date is the commit date, or `SOURCE_DATE_EPOCH`, never the wall clock.
The shown `(Release)` is the build type (`--with-build-type`; `custom` when you gave `CFLAGS`).

### Channel: local or official

* **local** (default): unsigned, for your own use.
* **official**: meant to be signed by the maintainers. `./configure --enable-official --with-signing-key=FPR`
  needs a clean tree and a known commit; `configure` and `make` both refuse otherwise. The binary then says
  `official (signed, key <fingerprint>; verify the .asc of the package)`. That is a claim: the detached signature of
  the package is the proof.

## tools/release.sh

```
tools/release.sh detect                         # machine, compiler, tools, which targets have a toolchain
tools/release.sh package [--targets=...] [--compress=gz|xz] [--outdir=DIR] [--check]
tools/release.sh package --official --sign-key=KEYID
tools/release.sh verify [dir] [--expect-key=FPR]
```

`package` runs `./configure --host=... --prefix=/` in a scratch directory per target (default `build-release/`),
`make`, then `make install-strip DESTDIR=staging`, and writes `dist/lizard-<version>-<target>.tar.gz` and `SHA256SUMS`.
`--targets=all` is the Linux matrix (gnu and musl on x86_64 and aarch64, armv7, riscv64); a target without a
toolchain (`<triplet>-gcc`, `zig`, or `clang --sysroot=`) is skipped, or an error with `--strict`. `--check` runs
`make check` for the native target; cross targets are not tested.

With GNU tar the tarballs are reproducible (sorted entries, fixed owner and mtime, `gzip -n`, dates from
`SOURCE_DATE_EPOCH` or the commit): the same commit built twice gives identical files. `package --official` also writes a
detached `*.asc` for every tarball and for `SHA256SUMS`; the key comes from `--sign-key`, `$LIZARD_SIGNING_KEY`, or your
first secret key. `verify` checks the checksums and signatures.

```
git tag v0.2.0 && git push --tags
tools/release.sh package --official --targets=all --sign-key=<KEYID>
tools/release.sh verify --expect-key=<FINGERPRINT>
```

## Source tarball

```
make dist         # lizard-<version>.tar.gz, with configure inside
make distcheck    # also unpacks it, builds out of tree, runs the tests, checks install and DESTDIR
```

`make dist` writes a `.buildinfo` (commit and kind) into the tarball, so a build from it, without git, reports the same
commit: kind `source`, version `0.1.0+g<sha>`.
