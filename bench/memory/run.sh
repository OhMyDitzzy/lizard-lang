#!/usr/bin/env bash
# Measures the memory benchmarks at two sizes and flags superlinear growth.
#
#   bench/memory/run.sh [path/to/lizard]
#
# Every script holds one size line (`let N = ...`, `let DEPTH = ...` or `let ... = ...` below), and
# this runner runs it at SIZE and at 2*SIZE (in a temp copy) with --heap-stats. Time that grows by
# more than ~2.5x when the input doubles is reported as suspicious: linear work should double.
#
# This lives outside bench/*.lz on purpose: the engine comparison of bench/run.sh must not pick it up.

LIZARD=${1:-./lizard}
HERE=$(cd "$(dirname "$0")" && pwd)
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

if [ ! -x "$LIZARD" ]; then
    echo "bench: no executable at $LIZARD (build first, or pass the path)" >&2
    exit 1
fi

to_ms() { # "3.512" -> 3512
    local sec=${1%.*} frac=${1#*.}
    frac="${frac}000"
    echo $((10#$sec * 1000 + 10#${frac:0:3}))
}

time_ms() { # file -> wall time in milliseconds
    local t
    TIMEFORMAT=%R
    t=$({ time "$LIZARD" --heap-stats "$1" >/dev/null 2>/dev/null; } 2>&1)
    to_ms "$t"
}

# size_var file: the size to double, or empty when the script has a fixed size.
size_var() {
    grep -m1 -E '^let N = [0-9]+$' "$1"
}

run_at() { # name file size
    local name=$1 file=$2 size=$3
    local copy="$TMP/$name.$size.lz"
    if [ -n "$size" ]; then
        sed -E "s/^let (N|DEPTH) = [0-9]+$/let \1 = $size/" "$file" >"$copy"
    else
        cp "$file" "$copy"
    fi
    time_ms "$copy"
}

printf '%-18s %9s %9s %8s\n' benchmark small large growth
for file in "$HERE"/*.lz; do
    name=$(basename "$file" .lz)
    line=$(size_var "$file")
    if [ -z "$line" ]; then
        size=0
    else
        size=$(printf '%s' "$line" | grep -oE '[0-9]+$')
    fi
    small=$(run_at "$name" "$file" "$size")
    large=$(run_at "$name" "$file" "$((size * 2))")
    if [ "$small" -gt 0 ]; then
        ratio=$((large * 100 / small))
    else
        ratio=0
    fi
    flag=""
    if [ "$ratio" -gt 250 ]; then
        flag="  SUPERLINEAR"
    fi
    printf '%-18s %8sms %8sms %5d.%02dx%s\n' "$name" "$small" "$large" $((ratio / 100)) $((ratio % 100)) "$flag"
done
