# Prerequisites

| Need | Minimum | For |
| --- | --- | --- |
| C compiler, C11 | gcc 5, clang 3.5 | everything |
| `make` | any POSIX make; GNU make is what is tested | the Autotools path |
| `autoconf` | 2.69 | only a git checkout (`./autogen.sh`) |
| `automake` | 1.16 | only a git checkout |
| `sh`, `awk`, `ar` | POSIX | the plain path (`tools/build-plain.sh`) needs only these and the compiler |
| `git` | any | optional: gives the build its commit and kind |
| `gpg`, `tar`, `gzip`/`xz` | | only `tools/release.sh` (gpg only for `--official`) |
| `valgrind` | | only `tools/valgrind-check.sh` |

A **release tarball** already contains `configure`: you need only a compiler and `make`.

## Debian, Ubuntu

```
sudo apt-get install build-essential autoconf automake git
```

## Fedora

```
sudo dnf install gcc make autoconf automake git
```

## Alpine

```
apk add build-base autoconf automake git
```

## macOS (Homebrew)

```
xcode-select --install
brew install autoconf automake
```

## Termux

```
pkg install clang make autoconf automake git
```

See [termux.md](termux.md). To skip Autotools entirely: `pkg install clang` and
[plain.md](plain.md).

## proot-distro

Install a distribution (`proot-distro install debian`, `proot-distro login debian`) and follow its
page above. Sanitizers do not work there: see [termux.md](termux.md).
