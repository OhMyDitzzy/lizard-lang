#!/bin/sh
# Negative test of the poison mode: it must catch the old link bug (item 1 of the link-safety
# work). The bug is put back with -DLZ_TEST_REINTRODUCE_LINK_BUG1 (memory.c: only the outermost
# release tells the link table), the suite's test_links is built in poison mode, and it must FAIL.
# If it passes, the poison mode is blind to that use-after-free and this script exits 1.
#
# usage: tests/poison_selftest.sh [build-dir]
#   (not part of make check; needs a configured source tree: ./autogen.sh first when from git,
#    and a compiler. The old variant is built in a scratch directory with
#    ./configure --enable-poison)
set -u
root=$(cd "$(dirname "$0")/.." && pwd)
build=${1:-"${TMPDIR:-/tmp}/lizard-poison-selftest.$$"}
[ -x "$root/configure" ] || { echo "no ./configure in $root: run ./autogen.sh first"; exit 2; }
mkdir -p "$build" || exit 2
build=$(cd "$build" && pwd)
(cd "$build" && "$root/configure" --enable-poison --with-build-type=debug \
    CFLAGS="-g -O0 -DLZ_TEST_REINTRODUCE_LINK_BUG1" >configure.log 2>&1) || { echo "configure failed (see $build/configure.log)"; exit 2; }
(cd "$build" && make test_links >make.log 2>&1) || { echo "build failed (see $build/make.log)"; exit 2; }
if (cd "$build" && ./test_links) >"$build/selftest.log" 2>&1; then
    echo "poison selftest FAILED: test_links passed with the old bug put back"
    exit 1
fi
if grep -q "0xDDDDDDDD\|Assertion" "$build/selftest.log"; then
    echo "poison selftest ok: the old bug is caught: $(grep -m1 'Assertion' "$build/selftest.log")"
else
    echo "poison selftest ok: test_links failed with the old bug (see $build/selftest.log)"
fi
exit 0
