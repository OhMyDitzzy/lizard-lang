#!/bin/sh
# Runs the link-related tests under Valgrind. Skips (exit 0) when valgrind is not installed.
# usage: tools/valgrind-check.sh [build-dir]    (default: the current directory, a configured build)
# It builds the three test programs with `make` when they are not there yet.
build=${1:-.}
if ! command -v valgrind >/dev/null 2>&1; then
    echo "valgrind not found: skipped"
    exit 0
fi
if [ ! -x "$build/test_links" ] || [ ! -x "$build/test_classes" ] || [ ! -x "$build/fuzz_links" ]; then
    make -C "$build" test_links test_classes fuzz_links >/dev/null || { echo "could not build the tests in $build"; exit 2; }
fi
status=0
for t in test_links test_classes fuzz_links; do
    echo "== $t"
    if [ "$t" = fuzz_links ]; then args="200"; else args=""; fi
    valgrind --error-exitcode=1 --leak-check=full -q "$build/$t" $args || status=1
done
exit $status
