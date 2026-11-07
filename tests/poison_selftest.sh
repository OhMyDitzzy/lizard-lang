#!/bin/sh
# Negative test of the poison mode: it must catch the old link bug (item 1 of the link-safety
# work). The bug is put back with -DLZ_TEST_REINTRODUCE_LINK_BUG1 (memory.c: only the outermost
# release tells the link table), the suite's test_links is built in poison mode, and it must FAIL.
# If it passes, the poison mode is blind to that use-after-free and this script exits 1.
#
# usage: tests/poison_selftest.sh [build-dir]       (not part of ctest; needs cmake and a compiler)
set -u
root=$(cd "$(dirname "$0")/.." && pwd)
build=${1:-"$root/build-poison-selftest"}
cmake -S "$root" -B "$build" -DCMAKE_BUILD_TYPE=Debug -DLZ_ENABLE_POISON=ON \
    -DCMAKE_C_FLAGS="-DLZ_TEST_REINTRODUCE_LINK_BUG1" >/dev/null || { echo "configure failed"; exit 2; }
cmake --build "$build" --target test_links >/dev/null || { echo "build failed"; exit 2; }
if "$build/bin/test_links" >"$build/selftest.log" 2>&1; then
    echo "poison selftest FAILED: test_links passed with the old bug put back"
    exit 1
fi
if grep -q "0xDDDDDDDD\|Assertion" "$build/selftest.log"; then
    echo "poison selftest ok: the old bug is caught: $(grep -m1 'Assertion' "$build/selftest.log")"
else
    echo "poison selftest ok: test_links failed with the old bug (see $build/selftest.log)"
fi
exit 0
