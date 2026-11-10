#!/bin/sh
# bench_scripts: the benchmark tooling in bench/ works, without timing anything.
#   - every bench/*.lz has exactly one size line and runs on the three engines with the same output
#   - the scripts pass bash -n
#   - the callgrind_annotate parser, compare.sh and report.sh work on the hand made files in tests/bench
#   - measure.sh flags different output (with a fake lizard) and reads GNU time (with a fake time)
# usage: tests/bench_scripts_check.sh [path/to/lizard]
here=$(cd "$(dirname "$0")" && pwd)
src=$(cd "$here/.." && pwd)
lizard=${1:-}
fail=0
bad() { printf 'FAIL %s\n' "$*"; fail=1; }

command -v bash >/dev/null 2>&1 || { echo "skipped: bench scripts need bash"; exit 77; }
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT INT TERM

for s in measure compare profile report ab; do
    bash -n "$src/bench/$s.sh" || bad "bash -n bench/$s.sh"
done

for f in "$src"/bench/*.lz; do
    n=$(grep -c -E '^let N = [0-9]+$' "$f")
    [ "$n" -eq 1 ] || bad "$(basename "$f"): $n size lines (want exactly 1)"
done

# parser
bash "$src/bench/profile.sh" --parse "$here/bench/annotate_new.txt" >"$tmp/new.txt"
want=$(printf '# total_ir 10000000\n60.00%%\t6000000\tlz_vm_run\n15.00%%\t1500000\tsrc/core/value.c:lz_value_free\n9.00%%\t900000\tsrc/core/table.c:table_find\n5.00%%\t500000\tmalloc\n4.00%%\t400000\t0x0000000000012345\n3.00%%\t300000\tfree\n')
[ "$(cat "$tmp/new.txt")" = "$want" ] || bad "profile --parse of annotate_new.txt"
bash "$src/bench/profile.sh" --parse "$here/bench/annotate_old.txt" >"$tmp/old.txt"
[ "$(head -n 1 "$tmp/old.txt")" = "# total_ir 2000" ] || bad "profile --parse of annotate_old.txt"
printf 'no totals here\n' >"$tmp/junk.txt"
bash "$src/bench/profile.sh" --parse "$tmp/junk.txt" >/dev/null 2>&1 && bad "profile --parse accepted junk"

# compare: a file against itself is all noise and 0%
d=$here/bench/data
bash "$src/bench/compare.sh" "$d/time-head.tsv" "$d/time-head.tsv" >"$tmp/self.md"
grep -q 'noise' "$tmp/self.md" || bad "compare self: no noise"
[ "$(grep -c '+0.0%' "$tmp/self.md")" -ge 6 ] || bad "compare self: changes are not 0%"
bash "$src/bench/compare.sh" "$d/head/instructions.tsv" "$d/head/instructions.tsv" >"$tmp/selfir.md"
grep -q '+0.0%' "$tmp/selfir.md" || bad "compare self (Ir)"
bash "$src/bench/compare.sh" "$d/base/instructions.tsv" "$d/head/instructions.tsv" >"$tmp/ir.md"
grep -q -- '-10.0%' "$tmp/ir.md" || bad "compare Ir: fib ref should be -10.0%"
bash "$src/bench/compare.sh" "$d/time-base.tsv" "$d/time-head.tsv" >"$tmp/time.md"
grep -q '| fib | ref | 700 | 610 | -12.9% |' "$tmp/time.md" || bad "compare time: fib ref"
grep -q '| fib | ge | 230 | 231 | +0.4% | noise |' "$tmp/time.md" || bad "compare time: fib ge should be noise"
grep -q 'Geometric mean of time, ref' "$tmp/time.md" || bad "compare time: no geometric mean"

# report
bash "$src/bench/report.sh" "$d" "$tmp/report.md" || bad "report.sh failed"
for h in '## Metadata' '## Smoke test' '## Time' '## Instructions (Ir)' '## Peak memory' '## Top functions (head)' '## Warnings'; do
    grep -q "^$h" "$tmp/report.md" || bad "report: missing heading $h"
done
grep -q 'OUTPUT DIFFERS' "$tmp/report.md" || bad "report: OUTPUT DIFFERS not in the warnings"
grep -q 'jit skipped' "$tmp/report.md" || bad "report: the JIT warning is missing"
grep -q 'Functions whose share of Ir changed most' "$tmp/report.md" || bad "report: no function changes"
[ "$(wc -c <"$tmp/report.md")" -lt 200000 ] || bad "report: over 200 KB"
# a head-only data dir still gives a report
mkdir "$tmp/headonly"
cp "$d/meta.txt" "$d/time-head.tsv" "$tmp/headonly/"
bash "$src/bench/report.sh" "$tmp/headonly" "$tmp/headonly.md" || bad "report.sh without base failed"
grep -q 'no base ref' "$tmp/headonly.md" || bad "report without base: no warning"
# the size limit cuts the function lists
MAX_BYTES=3000 TOPN=15 bash "$src/bench/report.sh" "$d" "$tmp/small.md" 2>/dev/null
[ "$(grep -c '^| [0-9.]*% |' "$tmp/small.md")" -lt "$(grep -c '^| [0-9.]*% |' "$tmp/report.md")" ] || bad "report: TOPN did not shrink"

# measure with a fake lizard: jit prints something else, and `time -v` is faked
cat >"$tmp/fake" <<'EOS'
#!/bin/sh
case "$*" in
    --version) echo "features jit=on"; exit 0 ;;
    *--jit=on*) echo "different" ;;
    *) echo "same" ;;
esac
EOS
chmod +x "$tmp/fake"
cat >"$tmp/faketime" <<'EOS'
#!/bin/sh
[ "$1" = "-v" ] || exit 1
shift
[ "$1" = "true" ] && exit 0
"$@"
echo "	Maximum resident set size (kbytes): 4321" >&2
EOS
chmod +x "$tmp/faketime"
TIME_BIN="$tmp/faketime" SCALE=1000 RUNS=2 ONLY=fib bash "$src/bench/measure.sh" "$tmp/fake" "$tmp/fake.tsv" 2>/dev/null
[ $? -eq 3 ] || bad "measure: exit status 3 expected for different output"
grep -q '^fib	jit	.*OUTPUT DIFFERS$' "$tmp/fake.tsv" || bad "measure: OUTPUT DIFFERS not flagged"
grep -q '^fib	ref	.*	4321	ok$' "$tmp/fake.tsv" || bad "measure: peak memory from time -v not read"
ENGINES=ref,ge TIME_BIN=/nonexistent SCALE=1000 RUNS=2 ONLY=fib bash "$src/bench/measure.sh" "$tmp/fake" "$tmp/fake2.tsv" 2>/dev/null || bad "measure: ref,ge on the fake"

# the real thing, when a lizard is given: smoke at a tiny size, same output everywhere
if [ -n "$lizard" ] && [ -x "$lizard" ]; then
    SCALE=1000 RUNS=1 bash "$src/bench/measure.sh" "$lizard" "$tmp/smoke.tsv" 2>/dev/null || bad "measure: smoke test failed"
    awk -F'	' 'NR > 1 && $7 != "ok" { print "FAIL smoke " $1 " on " $2 ": " $7; bad = 1 } END { exit bad }' "$tmp/smoke.tsv" || fail=1
    bash "$src/bench/compare.sh" "$tmp/smoke.tsv" "$tmp/smoke.tsv" >"$tmp/smoke.md" || bad "compare on the smoke TSV"
    ! grep -v -E 'noise|\| *$' "$tmp/smoke.md" | grep -q -E '^\| [a-z_]+ \| (ref|ge|jit) \|.*[-+][1-9][0-9.]*%' || bad "compare smoke against itself: a change shows"
fi

[ "$fail" -eq 0 ] && echo "bench scripts ok"
exit "$fail"
