#!/usr/bin/env bash
# Times a base build and a head build one benchmark at a time, taking turns, so that a slow
# stretch on a shared machine hits both the same way. Never parallel.
#
#   bench/ab.sh BASE_LIZARD HEAD_LIZARD OUT_DIR
#
# BASE_LIZARD may be `-` to measure the head alone. Writes OUT_DIR/time-base.tsv and
# OUT_DIR/time-head.tsv. RUNS, ENGINES, SCALE and ONLY work as in bench/measure.sh.
# Exit status: 0, or 3 when measure.sh found a failure or different output.

set -u
BASE=${1:-}
HEAD=${2:-}
OUT=${3:-}
[ -n "$BASE" ] && [ -n "$HEAD" ] && [ -n "$OUT" ] || {
    echo "usage: ab.sh BASE_LIZARD|- HEAD_LIZARD OUT_DIR" >&2
    exit 2
}
HERE=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$OUT"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

only=${ONLY:-}
bad=0
header=yes
for file in "$HERE"/*.lz; do
    name=$(basename "$file" .lz)
    if [ -n "$only" ]; then
        case ",$(printf '%s' "$only" | tr ' ' ','), " in *",$name,"*) ;; *) continue ;; esac
    fi
    for side in base head; do
        if [ "$side" = base ]; then
            [ "$BASE" = "-" ] && continue
            bin=$BASE
        else
            bin=$HEAD
        fi
        ONLY=$name bash "$HERE/measure.sh" "$bin" "$TMP/$side.tsv"
        [ $? -eq 3 ] && bad=1
        if [ "$header" = yes ]; then
            cp "$TMP/$side.tsv" "$OUT/time-$side.tsv"
        else
            tail -n +2 "$TMP/$side.tsv" >>"$OUT/time-$side.tsv"
        fi
    done
    header=no
done
[ "$bad" -eq 0 ] || exit 3
exit 0
