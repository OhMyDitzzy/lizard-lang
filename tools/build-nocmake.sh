#!/bin/bash
# Fallback build without cmake (used where cmake/ninja are missing): build-nocmake.sh BUILDDIR "CFLAGS" [test names...]
# usage: mk.sh BUILDDIR "EXTRA_CFLAGS"  [tests...]  -- fallback build without cmake
set -e
SRC=$(cd "$(dirname "$0")/.." && pwd); B=$1; EXTRA=$2; shift 2
mkdir -p $B/gen/lizard $B/obj $B/bin
cat > $B/gen/lizard/version.h <<'H'
#ifndef LIZARD_VERSION_H
#define LIZARD_VERSION_H
#define LZ_VERSION_MAJOR 0
#define LZ_VERSION_MINOR 1
#define LZ_VERSION_PATCH 0
#define LZ_VERSION_CORE "0.1.0"
#define LZ_VERSION_STRING "0.1.0+source"
#define LZ_BUILD_KIND "source"
#define LZ_BUILD_DIRTY 0
#define LZ_BUILD_DIRTY_HASH ""
#define LZ_BUILD_COMMIT "unknown"
#define LZ_BUILD_COMMIT_SHORT "unknown"
#define LZ_BUILD_DATE "unknown"
#define LZ_BUILD_CHANNEL "local"
#define LZ_BUILD_SIGNED 0
#define LZ_BUILD_SIGNER ""
#define LZ_BUILD_CONFIG "Debug"
#define LZ_BUILD_TARGET "x86_64-linux-gnu"
#define LZ_BUILD_COMPILER "gcc"
#define LZ_BUILD_JIT 1
#define LZ_BUILD_LTO 0
#define LZ_BUILD_SANITIZERS 0
#define LZ_BUILD_STATIC 0
#define LZ_VERSION_LONG "0.1.0+source"
#endif
H
CF="-std=c11 -g -O1 -Wall -Wextra -Wshadow -Wmissing-prototypes -I$SRC/include -I$SRC/builtin/include -I$SRC/builtin -I$SRC/src/engine -I$B/gen $EXTRA"
build_lib() { # name files...
  n=$1; shift; objs=""
  for f in "$@"; do o=$B/obj/$n-$(echo $f | tr / _).o; objs="$objs $o"
    if [ ! -f $o ] || [ $SRC/$f -nt $o ] || [ -n "$(find $SRC/include $SRC/builtin -name '*.h' -newer $o | head -1)" ] || [ -n "$(find $SRC/src -name '*.h' -newer $o | head -1)" ] ; then
      echo "gcc $f" >&2; gcc $CF -c $SRC/$f -o $o &
    fi
  done; wait; ar rcs $B/lib$n.a $objs; }
build_lib core $(cd $SRC; ls src/core/*.c)
build_lib frontend src/frontend/compiler.c src/frontend/lexer.c src/frontend/strlit.c
build_lib engine $(cd $SRC; ls src/engine/*.c | grep -v ge_jit_a64_unused)
build_lib builtin builtin/registry.c builtin/array/array.c builtin/array/higher.c builtin/method/casemap.c builtin/method/dict.c builtin/method/method.c builtin/method/number.c builtin/method/text.c builtin/convert/convert.c builtin/io/print.c
build_lib utils src/utils/argparse.c
L="$B/libengine.a $B/libfrontend.a $B/libbuiltin.a $B/libcore.a -lm"
gcc $CF $SRC/src/cli/main.c -o $B/bin/lizard $B/libengine.a $B/libfrontend.a $B/libbuiltin.a $B/libcore.a $B/libutils.a -lm
for t in "$@"; do
  gcc $CF $SRC/tests/$t.c -o $B/bin/$t $L 2>&1 | grep -E "error|warning: unused" || true
  case $t in *_ge) ;; esac
done
