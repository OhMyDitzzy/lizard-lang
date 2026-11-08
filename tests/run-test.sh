#!/bin/sh
# Runs one test of tests/tests.list. Used by `make check` (as LOG_COMPILER) and by
# tools/build-plain.sh check. Run it in the build directory:
#
#   sh /path/to/tests/run-test.sh NAME        (NAME may be given as ./NAME)
#
# Exit status: 0 pass, 77 skipped, anything else fails. LZ_SRCDIR overrides the source tree.
here=$(cd "$(dirname "$0")" && pwd)
srcdir=${LZ_SRCDIR:-$(cd "$here/.." && pwd)}
list=$srcdir/tests/tests.list
name=$(basename "${1:?usage: run-test.sh NAME}")

line=$(awk -v n="$name" '$0 !~ /^[ \t]*#/ && $1 == n { print; exit }' "$list")
[ -n "$line" ] || { echo "run-test: no test named '$name' in $list" >&2; exit 99; }
# shellcheck disable=SC2086
set -- $line
prog=$2 build=$3 envs=$4
shift 4

builddir=$(pwd)
args=
for a in "$@"; do
    [ "$a" = "-" ] && continue
    case $a in
        @lizard) a=$builddir/lizard ;;
        @buildinfo) a=$builddir/generated/BUILDINFO ;;
        @srcdir) a=$srcdir ;;
    esac
    args="$args $a"
done

if [ "$envs" != "-" ]; then
    for kv in $(printf '%s' "$envs" | tr ',' ' '); do export "${kv?}"; done
fi

case $build in
    script)
        case $(uname -s 2>/dev/null) in MINGW*|MSYS*|CYGWIN*) echo "skipped: shell tests need a POSIX platform"; exit 77 ;; esac
        if [ "$name" = skink_cli ] && ! grep -q '^jit=on' "$builddir/generated/BUILDINFO" 2>/dev/null; then
            echo "skipped: skink_cli needs the JIT (this build has none)"; exit 77
        fi
        set -- sh "$srcdir/$prog" ;;
    *) set -- "./$prog" ;;
esac

# shellcheck disable=SC2086
if command -v timeout >/dev/null 2>&1; then
    exec timeout 600 "$@" $args
fi
exec "$@" $args
