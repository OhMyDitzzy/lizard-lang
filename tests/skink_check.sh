#!/bin/sh
# Runs small programs through the reference interpreter, the Gecko interpreter and the Gecko
# JIT (compiled on first sight) and checks that the three agree on everything Skink touches:
# the output, the heap statistics, the collector's statistics, and that nothing is left alive.
# It is aimed at the paths the JIT back-ends changed: SET_UPVALUE (inline for numbers, strings,
# watched and open upvalues, the reference implementation for the rest) and the allocation hook.
#
#   tests/skink_check.sh path/to/lizard
#
# On a platform without a JIT the "jit is active" check fails on purpose: it would otherwise
# be a test of the GE interpreter run twice.

LZ=${1:?usage: skink_check.sh path/to/lizard}
dir=$(mktemp -d "${TMPDIR:-/tmp}/skink_check.XXXXXX") || exit 2
trap 'rm -rf "$dir"' EXIT INT TERM
failures=0
checks=0

fail() {
    failures=$((failures + 1))
    printf 'FAIL %s\n' "$*"
}

# ---- the programs -------------------------------------------------------------------------

# A closure assigned to a closed upvalue that held nil when its frame ended (slow path in the JIT).
cat >"$dir/set_closure.lz" <<'EOF'
fn tick() {
  let k = 0
  let g = fn() { return k }
  return 0
}
fn make() {
  let slot = nil
  let c = fn(self) {
    slot = self
    return 0
  }
  return c
}
let i = 0
while i < 1000 {
  let c = make()
  c(c)
  c = nil
  tick()
  i = i + 1
}
println("done")
EOF

# The same with an array holding the closure.
cat >"$dir/set_array.lz" <<'EOF'
fn tick() {
  let k = 0
  let g = fn() { return k }
  return 0
}
fn make() {
  let box = 0
  let c = fn(self) {
    box = [self]
    return 0
  }
  return c
}
let i = 0
while i < 1000 {
  let c = make()
  c(c)
  c = nil
  tick()
  i = i + 1
}
println("done")
EOF

# Hot stores of a number, a string and an array into a captured variable: inline in the JIT,
# except the first store of the array, which registers its upvalue (the rest are inline again).
for kind in number string array; do
    case $kind in
    number) store='s = k' ;;
    string) store='s = "idle"' ;;
    array) store='s = a' ;;
    esac
    cat >"$dir/hot_$kind.lz" <<EOF
fn make() {
  let s = nil
  let a = [1, 2, 3]
  return fn(k) {
    $store
    return k
  }
}
let c = make()
let i = 0
let t = 0
while i < 300000 {
  t = t + c(i)
  i = i + 1
}
println(t)
EOF
done

# Objects stored into an upvalue that is still open (inline in the JIT): the close reports it.
cat >"$dir/open_store.lz" <<'EOF'
fn make() {
  let slot = nil
  let f = fn() { return slot }
  slot = [1]
  slot = f
  return 0
}
make()
println("ok")
EOF

# Many dead cycles, one per frame exit.
cat >"$dir/dead_loop.lz" <<'EOF'
fn make() {
  let f = nil
  f = fn() { return f }
  return 0
}
let i = 0
while i < 50000 {
  make()
  i = i + 1
}
println("done")
EOF

# Typed arrays that cannot reach a closure are never suspects.
cat >"$dir/typed_matrix.lz" <<'EOF'
fn make(k: int) {
  let m: float[][] = [[1.0, 2.0, 3.0], [4.0, 5.0, 6.0], [7.0, 8.0, 9.0]]
  return fn(i: int, j: int) { return m[i][j] + k }
}
let keep = []
let i = 0
while i < 500 {
  push(keep, make(i))
  i = i + 1
}
let t = 0.0
let q = 0
while q < 500 {
  t = t + keep[q](q % 3, 1)
  q = q + 1
}
println(t)
EOF

# Dead cycles, then nothing but allocations: only the allocation hook can find them.
cat >"$dir/alloc_ticks.lz" <<'EOF'
fn make() {
  let f = nil
  f = fn() { return f }
  return f
}
let keep = []
let i = 0
while i < 200 {
  push(keep, make())
  i = i + 1
}
keep = nil
let j = 0
let n = 0
while j < 20000 {
  let a = [j, j, j]
  n = n + len(a)
  j = j + 1
}
println(n)
EOF

# Dead cycles of any size, one after another, with no --max-heap to ask for room: each holds a
# structure of 15000 small arrays (about 2.4 MB). The memory has to stay near one structure.
cat >"$dir/big_cycles.lz" <<'EOF'
fn make() {
  let big = []
  let i = 0
  while i < 15000 {
    push(big, [i, i])
    i = i + 1
  }
  let f = nil
  f = fn() {
    len(big)
    return f
  }
  return f
}
let r = 0
let n = 0
while r < 8 {
  let c = make()
  c = nil
  n = n + 1
  r = r + 1
}
println(n)
EOF

# Dead cycles that wait for their next poll while the program needs the room (--max-heap).
cat >"$dir/pressure.lz" <<'EOF'
fn make() {
  let f = nil
  f = fn() { return f }
  return f
}
let keep = []
let i = 0
while i < 3000 {
  push(keep, make())
  i = i + 1
}
keep = nil
let big = []
let j = 0
while j < 4000 {
  push(big, [j, j, j, j])
  j = j + 1
}
println(len(big))
EOF

# ---- the checks ---------------------------------------------------------------------------

# check NAME EXPECTED_OUTPUT EXTRA_LZ_OPTIONS PATTERN...
#   Runs NAME.lz in the three modes. PATTERNs are grep -E patterns that the "skink:" line of
#   every mode must match; "!skink" means there must be no such line at all.
check() {
    name=$1
    expected=$2
    extra=$3
    shift 3
    checks=$((checks + 1))
    printf '%s\n' "$expected" >"$dir/$name.expected"
    ok=1
    for mode in --engine=ref --jit=off --jit=eager; do
        case $mode in
        --engine=ref) tag=ref ;;
        --jit=off) tag=ge ;;
        --jit=eager) tag=jit ;;
        esac
        # shellcheck disable=SC2086
        "$LZ" $mode $extra --heap-stats "$dir/$name.lz" >"$dir/$name.$tag.out" 2>"$dir/$name.$tag.err"
        grep -E '^(heap|skink):' "$dir/$name.$tag.err" >"$dir/$name.$tag.stats"
        if ! cmp -s "$dir/$name.$tag.out" "$dir/$name.expected"; then
            fail "$name $mode: output is '$(head -c 80 "$dir/$name.$tag.out")', expected '$expected'"
            ok=0
        fi
        if ! grep -q ' 0 objects (0 bytes) still live' "$dir/$name.$tag.stats"; then
            fail "$name $mode: objects left alive: $(grep '^heap:' "$dir/$name.$tag.stats")"
            ok=0
        fi
        for pattern in "$@"; do
            if [ "$pattern" = '!skink' ]; then
                if grep -q '^skink:' "$dir/$name.$tag.stats"; then
                    fail "$name $mode: expected no skink line: $(grep '^skink:' "$dir/$name.$tag.stats")"
                    ok=0
                fi
            elif ! grep '^skink:' "$dir/$name.$tag.stats" | grep -Eq "$pattern"; then
                fail "$name $mode: skink line does not match '$pattern': $(grep '^skink:' "$dir/$name.$tag.stats")"
                ok=0
            fi
        done
    done
    # The engines must agree on every statistic, not just on the output.
    if ! cmp -s "$dir/$name.ref.stats" "$dir/$name.ge.stats" ||
        ! cmp -s "$dir/$name.ref.stats" "$dir/$name.jit.stats"; then
        fail "$name: the engines disagree on the statistics:"
        for tag in ref ge jit; do
            sed "s/^/       $tag  /" "$dir/$name.$tag.stats"
        done
        ok=0
    fi
    [ "$ok" = 1 ] && printf 'ok   %s\n' "$name"
}

printf 'lizard: %s\n' "$("$LZ" --version 2>&1 | head -1)"
printf 'machine: %s\n\n' "$(uname -sm)"

check set_closure 'done' '' '1000 of them by assignment' '1000 dead cycles freed'
check set_array 'done' '' '1000 of them by assignment' '1000 dead cycles freed'
check hot_number '44999850000' '' '!skink'
check hot_string '44999850000' '' '!skink'
check hot_array '44999850000' '' '1 of them by assignment'
check open_store 'ok' '' ' 1 dead cycles freed' ' 0 of them by assignment'
check dead_loop 'done' '' '50000 dead cycles freed'
check typed_matrix '127247.0' '' '!skink'
check alloc_ticks '60000' '' ' 200 dead cycles freed' '[1-9][0-9]* ticks from allocations'
check pressure '4000' '--max-heap=700000' '[1-9][0-9]* full passes'
check big_cycles '8' '' '[1-9][0-9]* full passes' '[1-9][0-9]* dead cycles freed'

# Leaked, eight structures would peak near 19 MB; collected as the heap grows, it stays near one.
checks=$((checks + 1))
for tag in ref ge jit; do
    peak=$(sed -n 's/^heap: .* peak \([0-9][0-9]*\) bytes.*/\1/p' "$dir/big_cycles.$tag.stats")
    if [ -z "$peak" ] || [ "$peak" -ge 8000000 ]; then
        fail "big_cycles ($tag): peak heap is '$peak' bytes, expected under 8000000"
    fi
done
printf 'ok   big_cycles stays bounded (peak %s bytes)\n' "$peak"

# --max-heap alone must be fatal without Skink and survivable with it.
checks=$((checks + 1))
if "$LZ" --skink=off --max-heap=700000 "$dir/pressure.lz" >/dev/null 2>"$dir/pressure.off.err"; then
    fail "pressure: the program survived --max-heap=700000 without Skink, so the check proves nothing"
elif ! grep -q 'heap limit' "$dir/pressure.off.err"; then
    fail "pressure: --skink=off failed, but not with a heap limit error: $(head -c 120 "$dir/pressure.off.err")"
else
    printf 'ok   pressure without skink fails with a heap limit error\n'
fi
if ! grep -q 'full passes ([1-9][0-9]* before --max-heap' "$dir/pressure.jit.err"; then
    fail "pressure: no pass ran before --max-heap=700000 refused an allocation"
fi

# The JIT must actually have run native code, or the three modes above are two.
checks=$((checks + 1))
"$LZ" --jit=eager --engine-stats "$dir/hot_number.lz" >/dev/null 2>"$dir/jit.err"
jit_line=$(grep -o 'jit: [0-9]* programs, [0-9]* bytes, [0-9]* entries' "$dir/jit.err" | head -1)
case $jit_line in
'' | 'jit: 0 programs'*) fail "the JIT did not run (--engine-stats: '$(grep -o 'engine.*' "$dir/jit.err" | head -1)')" ;;
*) printf 'ok   the JIT is active (%s)\n' "$jit_line" ;;
esac

printf '\n'
if [ "$failures" = 0 ]; then
    printf 'ALL PASSED (%s checks)\n' "$checks"
    exit 0
fi
printf '%s FAILURE(S) in %s checks\n' "$failures" "$checks"
exit 1
