#!/bin/sh
# sources_sync: the lists the two build paths read must agree with the tree and with each other.
#   - every source file in mk/sources.am exists, and every .c of src/ and builtin/ is listed
#   - Makefile.am names no source or test of its own (it only includes mk/sources.am, mk/tests.am)
#   - mk/tests.am is what tools/gen-tests-am.sh makes of tests/tests.list (not stale)
#   - every test program of tests/tests.list has its source, every tests/*.c is used by a test
#   - docs and examples installed by the lists are the docs and examples that exist
# usage: tools/check-sync.sh [source-dir]      (make check passes it)
src=${1:-$(cd "$(dirname "$0")/.." && pwd)}
cd "$src" || exit 2
fail=0
# Files that the CMake build never compiled or registered either (kept as they were; listing
# them is a decision for the maintainers, not for the build-system move).
unbuilt="builtin/error/error.c tests/test_options.c"
is_unbuilt() { case " $unbuilt " in *" $1 "*) return 0 ;; esac; return 1; }
bad() { printf 'FAIL %s\n' "$*"; fail=1; }

# the list of a variable of mk/sources.am, one item per line (same awk as tools/build-plain.sh)
list_of() {
    awk -v v="$1" '
        /^[ \t]*#/ { next }
        { s = $0; cont = (s ~ /\\[ \t]*$/); sub(/\\[ \t]*$/, "", s); buf = buf " " s
          if (!cont) { if (match(buf, /^ *[A-Za-z0-9_]+ *=/)) {
                name = substr(buf, 1, RLENGTH); sub(/ *=$/, "", name); gsub(/ /, "", name)
                if (name == v) { rest = substr(buf, RLENGTH + 1); n = split(rest, a, /[ \t]+/)
                    for (i = 1; i <= n; i++) if (a[i] != "") print a[i] } }
            buf = "" } }' mk/sources.am
}

all_listed=$(for v in lizard_core_SOURCES lizard_frontend_SOURCES lizard_engine_SOURCES \
                     lizard_builtin_SOURCES lizard_utils_SOURCES lizard_SOURCES; do list_of $v; done)
[ -n "$all_listed" ] || bad "mk/sources.am lists no sources"
for f in $all_listed; do [ -f "$f" ] || bad "mk/sources.am lists $f, which does not exist"; done
for f in $(find src builtin -name '*.c' | sort); do
    is_unbuilt "$f" && continue
    printf '%s\n' "$all_listed" | grep -qx "$f" || bad "$f is not in mk/sources.am"
done

if grep -v '^[ \t]*#' Makefile.am | grep -q '\.c\b'; then
    bad "Makefile.am names a .c file: sources belong in mk/sources.am, tests in tests/tests.list"
fi

sh tools/gen-tests-am.sh >"${TMPDIR:-/tmp}/lz-tests.am.$$" 2>/dev/null
cmp -s "${TMPDIR:-/tmp}/lz-tests.am.$$" mk/tests.am || bad "mk/tests.am is stale: run sh tools/gen-tests-am.sh > mk/tests.am (or ./autogen.sh)"
rm -f "${TMPDIR:-/tmp}/lz-tests.am.$$"

awk '$0 !~ /^[ \t]*#/ && NF { print $2, $3 }' tests/tests.list | while read -r prog build; do
    case $build in
        plain|ge|engine|utils)
            base=${prog%_ge}
            [ -f "tests/$base.c" ] || echo "tests/$base.c (for $prog)" ;;
        script) [ -f "$prog" ] || echo "$prog" ;;
    esac
done | while read -r missing; do bad "tests/tests.list needs $missing, which does not exist"; done
for f in tests/*.c; do
    base=$(basename "$f" .c)
    is_unbuilt "$f" && continue
    awk -v b="$base" '$0 !~ /^[ \t]*#/ && ($2 == b || $2 == b "_ge") { f = 1 } END { exit !f }' tests/tests.list \
        || bad "tests/$base.c is in no test of tests/tests.list"
done

for f in $(find docs -name '*.md' | sort) README.md; do
    list_of lizard_docs | grep -qx "$f" || bad "$f is not in lizard_docs (mk/sources.am)"
done
for f in $(list_of lizard_docs) $(list_of lizard_examples); do [ -f "$f" ] || bad "mk/sources.am lists $f, which does not exist"; done
for f in examples/*.lz; do list_of lizard_examples | grep -qx "$f" || bad "$f is not in lizard_examples"; done

if [ "$fail" -eq 0 ]; then
    echo "ok: the file lists, the test list and the tree agree"
fi
exit $fail
