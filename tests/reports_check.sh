#!/bin/sh
# Golden tests for error reports: every tests/reports/NAME.lz is run with --color=never on the
# reference interpreter, the Gecko interpreter and the JIT, and the full stderr must equal
# NAME.expected in all three. Files named color_*.lz run with --color=always instead.
#
#   tests/reports_check.sh path/to/lizard
#   LZ_UPDATE_REPORTS=1 tests/reports_check.sh path/to/lizard   # rewrite the .expected files;
#                                                               # review every changed file
LZ=${1:?usage: reports_check.sh path/to/lizard}
LZ=$(cd "$(dirname "$LZ")" && pwd)/$(basename "$LZ")
dir=$(cd "$(dirname "$0")/reports" && pwd) || exit 1
failures=0
count=0
tmp=$(mktemp -d) || exit 1
trap 'rm -rf "$tmp"' EXIT

cd "$dir" || exit 1
for src in *.lz; do
    name=${src%.lz}
    count=$((count + 1))
    case "$name" in
    color_*) color=always ;;
    *) color=never ;;
    esac
    "$LZ" --color=$color --engine=ref "$src" > /dev/null 2> "$tmp/ref"
    if [ -n "$LZ_UPDATE_REPORTS" ]; then
        cp "$tmp/ref" "$name.expected"
    fi
    for mode in "--engine=ref" "--engine=ge --jit=off" "--engine=ge --jit=eager"; do
        # shellcheck disable=SC2086
        "$LZ" --color=$color $mode "$src" > /dev/null 2> "$tmp/out"
        if [ ! -f "$name.expected" ]; then
            printf 'FAIL %s: no %s.expected\n' "$name" "$name"
            failures=$((failures + 1))
            break
        fi
        if ! cmp -s "$tmp/out" "$name.expected"; then
            printf 'FAIL %s (%s)\n' "$name" "$mode"
            diff "$name.expected" "$tmp/out" | head -10
            failures=$((failures + 1))
        fi
    done
done

if [ "$failures" -eq 0 ]; then
    printf 'ok   %d report cases on 3 engines\n' "$count"
    exit 0
fi
printf '%d failure(s) in %d cases\n' "$failures" "$count"
exit 1
