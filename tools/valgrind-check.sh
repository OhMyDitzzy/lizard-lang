#!/bin/sh
# Runs the link-related tests under Valgrind. Skips (exit 0) when valgrind is not installed.
# usage: tools/valgrind-check.sh [build-dir]    (default: build)
build=${1:-build}
if ! command -v valgrind >/dev/null 2>&1; then
    echo "valgrind not found: skipped"
    exit 0
fi
status=0
for t in test_links test_classes fuzz_links; do
    echo "== $t"
    if [ "$t" = fuzz_links ]; then args="200"; else args=""; fi
    valgrind --error-exitcode=1 --leak-check=full -q "$build/bin/$t" $args || status=1
done
exit $status
