#!/bin/sh
lizard=${1:?usage: run-coroutines.sh path/to/lizard}
lizard=$(cd "$(dirname "$lizard")" && pwd)/$(basename "$lizard")
here=$(cd "$(dirname "$0")/coroutines" && pwd)
status=0
captured=$(mktemp)
trap 'rm -f "$captured"' EXIT HUP INT TERM
for flags in "--engine=ref" "--jit=off" "--jit=eager"; do
    for file in "$here"/*.lz; do
        name=$(basename "$file")
        rc=0
        heap_flags=
        case "$name" in
            heap_small.lz) heap_flags=--max-heap=64M ;;
            bad_heap_large.lz) heap_flags=--max-heap=4M ;;
        esac
        (cd "$here" && "$lizard" $flags $heap_flags --color=never "$name") >"$captured" 2>&1 || rc=$?
        expected_status=0
        case "$name" in bad_arg_named.lz) expected_status=65 ;; bad_*) expected_status=70 ;; esac
        if [ "$rc" -ne "$expected_status" ] || ! cmp -s "$captured" "${file%.lz}.expected"; then
            echo "differs: $name $flags"
            status=1
        fi
    done
done
exit "$status"
