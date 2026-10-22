#!/usr/bin/env bash
# Runs every bench/*.lz on both engines and prints the best of N runs.
#
#   bench/run.sh [path/to/lizard] [runs]
#
# Defaults: build/bin/lizard, 3 runs. Also checks that both engines print the
# same output. Needs only bash: no bc, no awk.

LIZARD=${1:-build/bin/lizard}
RUNS=${2:-3}
HERE=$(cd "$(dirname "$0")" && pwd)

if [ ! -x "$LIZARD" ]; then
    echo "bench: no executable at $LIZARD (build first, or pass the path)" >&2
    exit 1
fi

to_ms() { # "3.512" -> 3512
    local sec=${1%.*} frac=${1#*.}
    frac="${frac}000"
    echo $((10#$sec * 1000 + 10#${frac:0:3}))
}

best_ms() { # engine file -> best wall time in milliseconds over RUNS runs
    local best="" t ms
    TIMEFORMAT=%R
    for ((n = 0; n < RUNS; n++)); do
        t=$({ time "$LIZARD" "--engine=$1" "$2" >/dev/null 2>&1; } 2>&1)
        ms=$(to_ms "$t")
        if [ -z "$best" ] || [ "$ms" -lt "$best" ]; then
            best=$ms
        fi
    done
    echo "$best"
}

secs() { printf '%d.%02d' $(($1 / 1000)) $((($1 % 1000) / 10)); }

printf '%-18s %9s %9s %9s\n' benchmark ref ge speedup
for file in "$HERE"/*.lz; do
    name=$(basename "$file" .lz)
    ref_out=$("$LIZARD" --engine=ref "$file" 2>&1)
    ge_out=$("$LIZARD" --engine=ge "$file" 2>&1)
    ref=$(best_ms ref "$file")
    ge=$(best_ms ge "$file")
    [ "$ge" -gt 0 ] || ge=1
    speedup=$((ref * 100 / ge))
    flag=""
    [ "$ref_out" = "$ge_out" ] || flag="  OUTPUT DIFFERS"
    printf '%-18s %8ss %8ss %6d.%02dx%s\n' "$name" "$(secs "$ref")" "$(secs "$ge")" $((speedup / 100)) $((speedup % 100)) "$flag"
done
