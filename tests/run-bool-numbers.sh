#!/bin/sh
# Runs tests/bool_numbers/*.lz on every engine. NAME.lz must print exactly NAME.expected. A bad_*.lz
# must fail with the exact diagnostic in bad_*.expected: status 65 for a compile error, 70 for a
# run time error.
# Usage: tests/run-bool-numbers.sh path/to/lizard
lizard=${1:?usage: run-bool-numbers.sh path/to/lizard}
lizard=$(cd "$(dirname "$lizard")" && pwd)/$(basename "$lizard")
here=$(cd "$(dirname "$0")/bool_numbers" && pwd)
status=0
captured=$(mktemp)
trap 'rm -f "$captured"' EXIT HUP INT TERM
for flags in "--engine=ref" "--jit=off" "--jit=eager" "--engine=ge"; do
    for file in "$here"/*.lz; do
        name=$(basename "$file")
        rc=0
        (cd "$here" && "$lizard" $flags --color=never "$name") >"$captured" 2>&1 || rc=$?
        expected_status=0
        case "$name" in
            bad_typed_let.lz | bad_param.lz | bad_index.lz | bad_string.lz | bad_slice.lz) expected_status=65 ;;
            bad_*) expected_status=70 ;;
        esac
        if [ "$rc" -ne "$expected_status" ] || ! cmp -s "$captured" "${file%.lz}.expected"; then
            echo "differs: $name $flags (status $rc)"
            status=1
        fi
    done
done
exit "$status"
