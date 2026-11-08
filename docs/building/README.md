# Building Lizard

Lizard builds with GNU Autotools (`configure` + `make`). A second path needs no Autotools and no
`make` at all: [`tools/build-plain.sh`](plain.md). Both read the same file list and test list, and
both give the same `lizard --version`.

## Quick start

From a git checkout:

```
./autogen.sh
./configure
make && make check
```

From a release tarball (`lizard-<version>.tar.gz`, made by `make dist`), no Autotools needed:

```
tar xf lizard-0.1.0.tar.gz && cd lizard-0.1.0
./configure && make && make check
```

Without Autotools and without make (Termux, Alpine, small machines):

```
sh tools/build-plain.sh check
```

`sudo make install` installs `lizard` under `/usr/local` (see [manual.md](manual.md) for `--prefix`).

## Pages

| Page | Contents |
| --- | --- |
| [prerequisites.md](prerequisites.md) | what to install, per system, and minimum versions |
| [manual.md](manual.md) | the build by hand, step by step; out-of-tree builds, `CC`, `CFLAGS`, `DESTDIR` |
| [options.md](options.md) | every `--enable-*`, `--with-*` and variable, with examples |
| [plain.md](plain.md) | `tools/build-plain.sh`: no Autotools, no make |
| [cross.md](cross.md) | cross compiling: `--host`, musl, `zig cc` |
| [termux.md](termux.md) | Android: native Termux, proot-distro, what does not work |
| [release.md](release.md) | version kinds, channels, `tools/release.sh`, signing, `make dist` |
| [troubleshooting.md](troubleshooting.md) | common errors and their fixes |

MSVC is not supported any more. MinGW through MSYS2 should work with the same commands but is not tested.
