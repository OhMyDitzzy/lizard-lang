#!/bin/sh
# Runs every project under tests/modules with the real lizard binary on every engine.
# A project is a directory with a main.lz and an expected.txt. A bad_*.lz next to it is a
# program that must fail, and bad_*.expected holds its exact diagnostic. An entry_NAME.expected
# holds the output of NAME.lz run directly as the entry file, where is_main is true.
# Usage: tests/run-modules.sh path/to/lizard
lizard=${1:?usage: run-modules.sh path/to/lizard}
lizard=$(cd "$(dirname "$lizard")" && pwd)/$(basename "$lizard")
here=$(cd "$(dirname "$0")" && pwd)
status=0
for flags in "--engine=ref" "--jit=off" "--jit=eager"; do
    ok=1
    for main in "$here"/modules/main.lz "$here"/modules/*/main.lz; do
        dir=$(dirname "$main")
        (cd "$dir" && "$lizard" $flags --color=never main.lz 2>&1) | cmp -s - "$dir/expected.txt" || { ok=0; echo "  differs: $main"; }
        for bad in "$dir"/bad_*.lz; do
            [ -e "$bad" ] || continue
            (cd "$dir" && "$lizard" $flags --color=never "$(basename "$bad")" 2>&1) | cmp -s - "${bad%.lz}.expected" || { ok=0; echo "  differs: $bad"; }
        done
        for entry in "$dir"/entry_*.expected; do
            [ -e "$entry" ] || continue
            name=$(basename "$entry" .expected)
            name=${name#entry_}.lz
            (cd "$dir" && "$lizard" $flags --color=never "$name" 2>&1) | cmp -s - "$entry" || { ok=0; echo "  differs: $dir/$name (entry)"; }
        done
    done
    if [ $ok = 1 ]; then echo "ok   $flags"; else echo "FAIL $flags"; status=1; fi
done
exit $status
