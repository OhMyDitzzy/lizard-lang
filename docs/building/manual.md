# Building by hand

Every step, without helper scripts.

## 1. Generate `configure` (git checkout only)

```
./autogen.sh
```

This runs `autoreconf -fi` (and regenerates `mk/tests.am` from `tests/tests.list`). A tarball skips this step.

## 2. Configure

```
./configure
```

Options are in [options.md](options.md). The default is a release build (`-O2 -DNDEBUG`), JIT on where it exists,
tests on. At the end `configure` prints a summary (tests, sanitizers, poison, werror, lto, jit).

## 3. Build

```
make            # or: make -j$(nproc)
./lizard --version
```

## 4. Test

```
make check                                   # all tests; results in test-suite.log
make check -j$(nproc)                        # in parallel
make check TESTS="test_links test_links_ge"  # only these tests
```

Test names are the names in `tests/tests.list`. Each test is limited to 600 seconds when `timeout` exists.
Cross builds (`--host=...`) print "skipped" instead.

## 5. Install

```
sudo make install                        # /usr/local/bin/lizard, docs in share/doc/lizard, examples in share/lizard
make install DESTDIR=/tmp/stage          # into a staging tree (packagers)
make install-strip                       # stripped binary
```

## Build outside the source tree

```
mkdir b && cd b
../configure
make && make check
```

The source tree stays clean. A build directory inside the tree does not make `lizard --version` say `dirty`.

## Another compiler, your own flags

```
./configure CC=clang
./configure CFLAGS="-O3 -march=native"     # your CFLAGS replace the default -O2; warnings stay on
```

The warning flags live in `AM_CFLAGS`, so `make CFLAGS=...` never loses them.

## Install without root

```
./configure --prefix=$HOME/.local
make && make install
export PATH=$HOME/.local/bin:$PATH
```

## Other targets

`make bench` runs `bench/*.lz` on both engines. `make dist` writes the source tarball, `make distcheck` also
builds and tests it in a scratch directory. `make clean`, `make distclean`.
