#!/bin/sh
# Prints the difference between two test lists; empty output means they agree.
#   tools/compare-test-lists.sh [--ignore=NAME]... CTEST_DIR MAKE_DIR
# CTEST_DIR: a CMake build directory (names from `ctest -N`).
# MAKE_DIR:  an Autotools build directory where `make check` has run (names from its *.trs files).
# sources_sync exists only in the Autotools build (it guards the shared lists) and is ignored.
ignore=sources_sync
while [ $# -gt 2 ]; do case $1 in --ignore=*) ignore="$ignore ${1#*=}" ;; esac; shift; done
ctest_dir=${1:?usage: compare-test-lists.sh [--ignore=NAME] CTEST_DIR MAKE_DIR}
make_dir=${2:?usage: compare-test-lists.sh [--ignore=NAME] CTEST_DIR MAKE_DIR}
tmp=${TMPDIR:-/tmp}/lz-cmp.$$
(cd "$ctest_dir" && ctest -N | sed -n 's/^ *Test *#[0-9]*: //p') | sort >"$tmp.a"
(cd "$make_dir" && for f in *.trs; do basename "$f" .trs; done) | sort >"$tmp.b"
for n in $ignore; do grep -vx "$n" "$tmp.b" >"$tmp.c"; mv "$tmp.c" "$tmp.b"; done
echo "ctest: $(wc -l <"$tmp.a") tests; make check: $(wc -l <"$tmp.b") tests (ignored: $ignore)" >&2
diff "$tmp.a" "$tmp.b" | sed 's/^</only in ctest:/; s/^>/only in make check:/' | grep '^only'
rc=0; [ -z "$(diff "$tmp.a" "$tmp.b")" ] || rc=1
rm -f "$tmp.a" "$tmp.b" "$tmp.c"
exit $rc
