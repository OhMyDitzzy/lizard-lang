# Cross compiling

```
./configure --host=aarch64-linux-gnu CC=aarch64-linux-gnu-gcc
make
```

Install the cross compiler first (`apt-get install gcc-aarch64-linux-gnu`). The tests cannot run on the build
machine, so `make check` prints "skipped" without failing. Test the result on the target.

## musl

```
./configure --host=x86_64-linux-musl CC=x86_64-linux-musl-gcc --enable-static-binary
make
```

or build on Alpine, where musl is the native C library.

## zig cc

```
./configure --host=aarch64-linux-gnu CC="zig cc -target aarch64-linux-gnu"
```

Some `configure` versions dislike a compiler with arguments; then make a wrapper script
(`#!/bin/sh` / `exec zig cc -target aarch64-linux-gnu "$@"`) and give its path as `CC`.
`tools/release.sh` does this for you and builds every target that has a toolchain (see [release.md](release.md)).

## JIT

The JIT exists for x86_64 and aarch64 on Linux and macOS. Other targets get `-DLZ_NO_JIT` and the interpreter.
