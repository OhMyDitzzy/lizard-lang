#!/usr/bin/env bash
# Counts instructions with callgrind: a number that does not vary from run to run.
#
#   bench/profile.sh [path/to/lizard] [out-dir]
#   bench/profile.sh --parse callgrind_annotate-output.txt
#
# Every bench/*.lz runs at a small size under `valgrind --tool=callgrind` on the engines ref and
# ge (the interpreter, --jit=off). Defaults: ./lizard, out-dir profile-out. It writes
#
#   out-dir/instructions.tsv           benchmark engine ir      (total Ir of the program)
#   out-dir/profile/<name>.<engine>.txt  the top 15 functions: first line `# total_ir N`, then
#                                      `percent<TAB>ir<TAB>function`
#   out-dir/allocations.tsv            benchmark blocks bytes gmax_bytes gmax_blocks (ref only, from
#                                      valgrind --tool=dhat: total blocks and bytes allocated, and
#                                      the bytes and blocks alive at the peak, t-gmax)
#   out-dir/warnings.txt               what was left out and why
#
# Environment: SCALE (default 20) divides the size line of every script, ONLY lists names,
# ENGINES is a comma list of ref, ge (default ref,ge). The JIT is always skipped: callgrind
# cannot name the machine code it generates, and the code is invisible to it unless valgrind is
# told to watch for self-modifying code, so Ir would not be comparable with the interpreters.
#
# --parse reads the text of callgrind_annotate and prints the same normalized form, which is how
# the parser is tested without valgrind (tests/bench_scripts_check.sh). --parse-dhat does the same
# for the summary dhat prints on stderr and prints `blocks bytes gmax_bytes gmax_blocks`.
#
# Needs bash, awk and valgrind (valgrind only when it measures).

set -u

TOP=15

# parse_annotate: callgrind_annotate text on stdin -> `# total_ir N` and the top functions
parse_annotate() {
    awk -v top="$TOP" '
        function ir(s) { gsub(/,/, "", s); return s + 0 }
        /PROGRAM TOTALS/ && total == "" {
            if (match($0, /^[ \t]*[0-9][0-9,]*/)) total = ir(substr($0, RSTART, RLENGTH))
            next
        }
        state == 0 && /^[ \t]*Ir[ \t]+file:function/ { state = 1; next }
        state == 1 && /^-+$/ { next }
        state == 1 {
            if (!match($0, /^[ \t]*[0-9][0-9,]*[ \t]+\([ ]*[0-9.]+%\)[ \t]+/)) { if (count > 0) state = 2; next }
            head = substr($0, 1, RLENGTH); name = substr($0, RLENGTH + 1)
            match(head, /[0-9][0-9,]*/); n = substr(head, RSTART, RLENGTH)
            match(head, /\([ ]*[0-9.]+%\)/); p = substr(head, RSTART, RLENGTH)
            gsub(/[() %]/, "", p)
            sub(/[ \t]+\[[^]]*\]$/, "", name)
            sub(/^\?\?\?:/, "", name)
            while (sub(/^\.\.?\//, "", name)) { }
            if (count < top) { count++; line[count] = sprintf("%.2f%%\t%d\t%s", p, ir(n), name) }
            else state = 2
        }
        END {
            if (total == "") exit 1
            printf "# total_ir %d\n", total
            for (i = 1; i <= count; i++) print line[i]
        }'
}

# parse_dhat: the stderr of valgrind --tool=dhat on stdin -> `blocks bytes gmax_bytes gmax_blocks`
parse_dhat() {
    awk '
        function num(s) { gsub(/,/, "", s); return s + 0 }
        { sub(/^==[0-9]+== /, "") }
        /^Total:[ \t]+[0-9,]+ bytes in [0-9,]+ blocks/ && total == "" {
            bytes = num($2); blocks = num($5); total = 1
        }
        /^At t-gmax:[ \t]+[0-9,]+ bytes in [0-9,]+ blocks/ && peak == "" {
            gbytes = num($3); gblocks = num($6); peak = 1
        }
        END {
            if (total == "" || peak == "") exit 1
            printf "%d\t%d\t%d\t%d\n", blocks, bytes, gbytes, gblocks
        }'
}

if [ "${1:-}" = "--parse-dhat" ]; then
    [ -r "${2:-}" ] || { echo "usage: profile.sh --parse-dhat FILE" >&2; exit 2; }
    parse_dhat <"$2"
    exit $?
fi

if [ "${1:-}" = "--parse" ]; then
    [ -r "${2:-}" ] || { echo "usage: profile.sh --parse FILE" >&2; exit 2; }
    parse_annotate <"$2"
    exit $?
fi

LIZARD=${1:-./lizard}
OUTDIR=${2:-profile-out}
SCALE=${SCALE:-20}
ONLY=${ONLY:-}
ENGINES=${ENGINES:-ref,ge}
HERE=$(cd "$(dirname "$0")" && pwd)

say() { printf '%s\n' "$*" >&2; }
die() { say "profile: $*"; exit 2; }

case $SCALE in '' | *[!0-9]* | 0) die "SCALE must be a positive integer, got '$SCALE'" ;; esac
[ -x "$LIZARD" ] || die "no executable at $LIZARD (build first, or pass the path)"
command -v valgrind >/dev/null 2>&1 || die "valgrind not found"
command -v callgrind_annotate >/dev/null 2>&1 || die "callgrind_annotate not found"

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$OUTDIR/profile"
: >"$OUTDIR/warnings.txt"
warn() { say "profile: $*"; printf '%s\n' "$*" >>"$OUTDIR/warnings.txt"; }

engines=()
for e in $(printf '%s' "$ENGINES" | tr ',' ' '); do
    case $e in
        ref | ge) engines+=("$e") ;;
        jit) ;;
        *) die "unknown engine '$e' (ref, ge)" ;;
    esac
done
[ ${#engines[@]} -gt 0 ] || die "no engine left to profile"
warn "jit skipped: callgrind does not see the generated machine code by function, so its Ir is not comparable"

engine_args() { case $1 in ref) echo "--engine=ref" ;; ge) echo "--engine=ge --jit=off" ;; esac; }

wanted() {
    [ -z "$ONLY" ] && return 0
    local n
    for n in $(printf '%s' "$ONLY" | tr ',' ' '); do
        [ "$n" = "$1" ] && return 0
    done
    return 1
}

printf 'benchmark\tengine\tir\n' >"$OUTDIR/instructions.tsv"
bad=0
for file in "$HERE"/*.lz; do
    name=$(basename "$file" .lz)
    wanted "$name" || continue
    size=$(grep -m1 -E '^let N = [0-9]+$' "$file" | grep -oE '[0-9]+$')
    if [ -z "$size" ]; then
        warn "$name skipped: no size line to scale"
        continue
    fi
    size=$((size / SCALE))
    [ "$size" -ge 1 ] || size=1
    sed -E "s/^let N = [0-9]+$/let N = $size/" "$file" >"$TMP/$name.lz"
    for e in "${engines[@]}"; do
        rm -f "$TMP/cg.out"
        # shellcheck disable=SC2086
        valgrind --tool=callgrind --callgrind-out-file="$TMP/cg.out" -q \
            "$LIZARD" $(engine_args "$e") "$TMP/$name.lz" >/dev/null 2>"$TMP/vg.err"
        rc=$?
        if [ "$rc" -ne 0 ] || [ ! -s "$TMP/cg.out" ]; then
            warn "$name on $e skipped: valgrind run failed (exit $rc)"
            printf '%s\t%s\t-\n' "$name" "$e" >>"$OUTDIR/instructions.tsv"
            bad=1
            continue
        fi
        callgrind_annotate "$TMP/cg.out" >"$TMP/annotate.txt" 2>/dev/null
        if ! parse_annotate <"$TMP/annotate.txt" >"$TMP/parsed.txt"; then
            warn "$name on $e skipped: could not read the callgrind_annotate output"
            printf '%s\t%s\t-\n' "$name" "$e" >>"$OUTDIR/instructions.tsv"
            bad=1
            continue
        fi
        cp "$TMP/parsed.txt" "$OUTDIR/profile/$name.$e.txt"
        total=$(awk '/^# total_ir/ { print $3 }' "$TMP/parsed.txt")
        printf '%s\t%s\t%s\n' "$name" "$e" "$total" >>"$OUTDIR/instructions.tsv"
        say "$(printf '%-18s %-4s Ir %s' "$name" "$e" "$total")"
    done
done

# Allocations: how much the reference engine allocates, to judge the memory management. The
# JIT and ge allocate the same objects, so ref alone says enough. dhat prints its summary to
# stderr, so there is no -q here.
have_ref=no
for e in "${engines[@]}"; do [ "$e" = ref ] && have_ref=yes; done
if [ "$have_ref" = yes ]; then
    printf 'benchmark\tblocks\tbytes\tgmax_bytes\tgmax_blocks\n' >"$OUTDIR/allocations.tsv"
    for file in "$HERE"/*.lz; do
        name=$(basename "$file" .lz)
        wanted "$name" || continue
        [ -s "$TMP/$name.lz" ] || continue
        valgrind --tool=dhat --dhat-out-file="$TMP/dhat.out" \
            "$LIZARD" --engine=ref "$TMP/$name.lz" >/dev/null 2>"$TMP/dhat.err"
        rc=$?
        if [ "$rc" -ne 0 ] || ! parse_dhat <"$TMP/dhat.err" >"$TMP/dhat.txt"; then
            warn "$name allocations skipped: dhat run failed (exit $rc)"
            printf '%s\t-\t-\t-\t-\n' "$name" >>"$OUTDIR/allocations.tsv"
            bad=1
            continue
        fi
        printf '%s\t%s\n' "$name" "$(cat "$TMP/dhat.txt")" >>"$OUTDIR/allocations.tsv"
        say "$(printf '%-18s dhat %s' "$name" "$(awk -F'\t' '{ printf "%s blocks, %s bytes, peak %s bytes in %s blocks", $1, $2, $3, $4 }' "$TMP/dhat.txt")")"
    done
fi

[ "$bad" -eq 0 ] || exit 3
exit 0
