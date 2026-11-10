#!/usr/bin/env bash
# Times every bench/*.lz on each engine and writes a TSV.
#
#   bench/measure.sh [path/to/lizard] [out.tsv]
#
# Defaults: ./lizard, TSV on stdout (progress goes to stderr). Options come from the environment:
#
#   RUNS     timed runs per benchmark and engine (default 7)
#   ENGINES  comma list of ref, ge, jit (default ref,ge,jit)
#              ref = --engine=ref, ge = --engine=ge --jit=off, jit = --engine=ge --jit=on
#   SCALE    divides the size line `let N = ...` of every script (default 1, smoke tests use 1000)
#   ONLY     comma or space list of benchmark names (default all)
#   TIME_BIN GNU time, used for the peak memory (default /usr/bin/time)
#
# Columns: benchmark engine min_ms median_ms dev_ms peak_kb status
# status is ok, OUTPUT DIFFERS, FAILED(code) or skipped(reason). Missing numbers are `-`.
# The peak is the maximum resident set from `time -v`, or the peak heap bytes of --heap-stats
# (in KB) when GNU time is not there. It comes from one extra run that is not timed.
# Exit status: 0, or 3 when some benchmark failed or an engine printed different output.
#
# Needs only bash and awk.

set -u

LIZARD=${1:-./lizard}
OUT=${2:-}
RUNS=${RUNS:-7}
ENGINES=${ENGINES:-ref,ge,jit}
SCALE=${SCALE:-1}
ONLY=${ONLY:-}
TIME_BIN=${TIME_BIN:-/usr/bin/time}
HERE=$(cd "$(dirname "$0")" && pwd)

say() { printf '%s\n' "$*" >&2; }
die() { say "measure: $*"; exit 2; }

case $RUNS in '' | *[!0-9]* | 0) die "RUNS must be a positive integer, got '$RUNS'" ;; esac
case $SCALE in '' | *[!0-9]* | 0) die "SCALE must be a positive integer, got '$SCALE'" ;; esac
[ -x "$LIZARD" ] || die "no executable at $LIZARD (build first, or pass the path)"

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

engine_args() { # engine -> options of lizard
    case $1 in
        ref) echo "--engine=ref" ;;
        ge) echo "--engine=ge --jit=off" ;;
        jit) echo "--engine=ge --jit=on" ;;
        *) return 1 ;;
    esac
}

# the engine list, without a jit when this build has none
engines=()
for e in $(printf '%s' "$ENGINES" | tr ',' ' '); do
    engine_args "$e" >/dev/null || die "unknown engine '$e' (ref, ge, jit)"
    if [ "$e" = jit ] && ! "$LIZARD" --version 2>/dev/null | grep -q 'jit=on'; then
        say "measure: jit engine skipped, this build has jit=off"
        continue
    fi
    engines+=("$e")
done
[ ${#engines[@]} -gt 0 ] || die "no engine left to run"

have_time_v=no
if [ -x "$TIME_BIN" ] && "$TIME_BIN" -v true >/dev/null 2>&1; then
    have_time_v=yes
fi

to_ms() { # "3.512" -> 3512
    local sec=${1%.*} frac=${1#*.}
    frac="${frac}000"
    echo $((10#$sec * 1000 + 10#${frac:0:3}))
}

wall_ms() { # args... -> wall time of one run in milliseconds
    local t
    TIMEFORMAT=%R
    t=$({ time "$@" >/dev/null 2>&1; } 2>&1)
    to_ms "$t"
}

peak_kb() { # args... -> peak memory in KB, or `-`
    local kb=""
    if [ "$have_time_v" = yes ]; then
        "$TIME_BIN" -v "$@" >/dev/null 2>"$TMP/mem.err"
        kb=$(awk -F: '/Maximum resident set size/ { gsub(/[ \t]/, "", $2); print $2; exit }' "$TMP/mem.err")
    else
        "$LIZARD_BIN" --heap-stats "${@:2}" >/dev/null 2>"$TMP/mem.err"
        kb=$(awk '/^heap:/ { for (i = 1; i < NF; i++) if ($i == "peak") { print int(($(i + 1) + 1023) / 1024); exit } }' "$TMP/mem.err")
    fi
    echo "${kb:--}"
}

# stats: ms values one per line on stdin -> "min median dev"
stats() {
    sort -n | awk '
        { v[NR] = $1; sum += $1 }
        END {
            n = NR; if (n == 0) { print "- - -"; exit }
            med = (n % 2) ? v[(n + 1) / 2] : (v[n / 2] + v[n / 2 + 1]) / 2
            mean = sum / n; ss = 0
            for (i = 1; i <= n; i++) ss += (v[i] - mean) ^ 2
            dev = (n > 1) ? sqrt(ss / (n - 1)) : 0
            printf "%d %.1f %.1f\n", v[1], med, dev
        }'
}

wanted() { # name -> 0 when ONLY allows it
    [ -z "$ONLY" ] && return 0
    local n
    for n in $(printf '%s' "$ONLY" | tr ',' ' '); do
        [ "$n" = "$1" ] && return 0
    done
    return 1
}

# scaled_copy name file -> path of the script to run, or empty when it cannot be scaled
scaled_copy() {
    local name=$1 file=$2 copy="$TMP/$1.lz" size
    if [ "$SCALE" -eq 1 ]; then
        cp "$file" "$copy"
        echo "$copy"
        return
    fi
    size=$(grep -m1 -E '^let N = [0-9]+$' "$file" | grep -oE '[0-9]+$')
    [ -n "$size" ] || return 0
    size=$((size / SCALE))
    [ "$size" -ge 1 ] || size=1
    sed -E "s/^let N = [0-9]+$/let N = $size/" "$file" >"$copy"
    echo "$copy"
}

[ -n "$OUT" ] && exec 3>"$OUT" || exec 3>&1
printf 'benchmark\tengine\tmin_ms\tmedian_ms\tdev_ms\tpeak_kb\tstatus\n' >&3

LIZARD_BIN=$LIZARD
bad=0
for file in "$HERE"/*.lz; do
    name=$(basename "$file" .lz)
    wanted "$name" || continue
    script=$(scaled_copy "$name" "$file")
    if [ -z "$script" ]; then
        say "measure: $name skipped, no size line to scale"
        for e in "${engines[@]}"; do
            printf '%s\t%s\t-\t-\t-\t-\tskipped(no size line)\n' "$name" "$e" >&3
        done
        continue
    fi
    refout="" refrc=""
    first=yes
    for e in "${engines[@]}"; do
        # shellcheck disable=SC2086
        args=$(engine_args "$e")
        out=$("$LIZARD" $args "$script" 2>&1)
        rc=$?
        status=ok
        if [ "$rc" -ne 0 ]; then
            status="FAILED($rc)"
        elif [ "$first" = yes ]; then
            refout=$out refrc=$rc
            first=no
        elif [ "$out" != "$refout" ] || [ "$rc" != "$refrc" ]; then
            status="OUTPUT DIFFERS"
        fi
        if [ "$status" != ok ]; then
            bad=1
            say "measure: $name on $e: $status"
            [ "$status" = "OUTPUT DIFFERS" ] || printf '%s\n' "$out" | head -3 >&2
            printf '%s\t%s\t-\t-\t-\t-\t%s\n' "$name" "$e" "$status" >&3
            continue
        fi
        times=""
        for ((n = 0; n < RUNS; n++)); do
            # shellcheck disable=SC2086
            times="$times$(wall_ms "$LIZARD" $args "$script")"$'\n'
        done
        read -r mn med dev <<<"$(printf '%s' "$times" | stats)"
        # shellcheck disable=SC2086
        kb=$(peak_kb "$LIZARD" $args "$script")
        printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$name" "$e" "$mn" "$med" "$dev" "$kb" "$status" >&3
        say "$(printf '%-18s %-4s median %8s ms  dev %6s  peak %s KB' "$name" "$e" "$med" "$dev" "$kb")"
    done
done

[ "$bad" -eq 0 ] || exit 3
exit 0
