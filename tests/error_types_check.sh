#!/bin/sh
# The command line side of declared error kinds: the report of an error that nobody caught, with its
# "= caused by:" lines (plain and with --color=always), and one program through the reference
# interpreter, the Gecko interpreter and the JIT (the output and what is left alive must agree).
#
#   tests/error_types_check.sh path/to/lizard

LZ=${1:?usage: error_types_check.sh path/to/lizard}
failures=0
tmp=$(mktemp -d) || exit 1
trap 'rm -rf "$tmp"' EXIT

fail() {
    failures=$((failures + 1))
    printf 'FAIL %s\n' "$*"
}
ok() { printf 'ok   %s\n' "$*"; }

# --- an uncaught chain
cat > "$tmp/chain.lz" <<'LZ'
error NotFound : IOError
error ParseError : ValueError
fn load() {
    throw NotFound("config.lz is missing")
}
fn parse() {
    try { load() } catch e { throw ParseError("line 3 is broken", cause: e, data: {"line": 3}) }
}
parse()
LZ
"$LZ" "$tmp/chain.lz" > "$tmp/out" 2> "$tmp/err"
code=$?
[ "$code" -ne 0 ] && ok "an uncaught error fails the run" || fail "exit code 0"
grep -q '^error: ParseError: line 3 is broken' "$tmp/err" && ok "main report" || fail "main report: $(cat "$tmp/err")"
grep -q '^  = caused by: NotFound: config.lz is missing$' "$tmp/err" && ok "caused by line" || fail "caused by line: $(cat "$tmp/err")"
n=$(grep -c 'caused by' "$tmp/err")
[ "$n" -eq 1 ] && ok "one cause, one line" || fail "$n caused by lines"

"$LZ" --color=always "$tmp/chain.lz" > /dev/null 2> "$tmp/errc"
esc=$(printf '\033')
grep -q "${esc}\[1;94m=${esc}\[0m ${esc}\[1;36mcaused by${esc}\[0m${esc}\[1m: ${esc}\[0mNotFound: config.lz is missing" "$tmp/errc" \
    && ok "caused by with color" || fail "caused by with color: $(cat -v "$tmp/errc")"
"$LZ" --color=never "$tmp/chain.lz" > /dev/null 2> "$tmp/errn"
grep -q "$esc" "$tmp/errn" && fail "escape codes with --color=never" || ok "no escape codes with --color=never"

# --- no cause, no line
cat > "$tmp/plain.lz" <<'LZ'
error Plain
throw Plain("alone")
LZ
"$LZ" "$tmp/plain.lz" > /dev/null 2> "$tmp/err"
grep -q 'caused by' "$tmp/err" && fail "caused by without a cause" || ok "no caused by without a cause"

# --- a chain longer than the limit is cut with "..."
cat > "$tmp/long.lz" <<'LZ'
let e = IOError("cause 0")
for i in range(1, 20) { e = IOError($"cause {i}", cause: e) }
throw e
LZ
"$LZ" "$tmp/long.lz" > /dev/null 2> "$tmp/err"
n=$(grep -c 'caused by' "$tmp/err")
[ "$n" -eq 8 ] && ok "at most 8 causes" || fail "$n caused by lines (cat: $(cat "$tmp/err"))"
grep -q '^  = \.\.\.$' "$tmp/err" && ok "the cut is marked" || fail "no ... line"

# --- the same program in every mode
cp "$(dirname "$0")/../examples/errors.lz" "$tmp/examples.lz" 2>/dev/null
for prog in "$tmp/examples.lz"; do
    [ -f "$prog" ] || { fail "examples/errors.lz is missing"; continue; }
    "$LZ" --engine=ref --heap-stats "$prog" > "$tmp/ref.out" 2> "$tmp/ref.err"
    "$LZ" --jit=off --heap-stats "$prog" > "$tmp/off.out" 2> "$tmp/off.err"
    "$LZ" --jit=eager --heap-stats "$prog" > "$tmp/eager.out" 2> "$tmp/eager.err"
    cmp -s "$tmp/ref.out" "$tmp/off.out" && ok "ref and jit=off print the same" || fail "ref and jit=off differ"
    cmp -s "$tmp/ref.out" "$tmp/eager.out" && ok "ref and jit=eager print the same" || fail "ref and jit=eager differ"
    cmp -s "$tmp/ref.err" "$tmp/off.err" && ok "stderr (heap stats) ref = off" || fail "stderr differs ref/off"
    cmp -s "$tmp/ref.err" "$tmp/eager.err" && ok "stderr (heap stats) ref = eager" || fail "stderr differs ref/eager"
    if grep -q ' 0 objects (0 bytes) still live' "$tmp/ref.err"; then ok "no object left alive"; else fail "objects left alive: $(cat "$tmp/ref.err")"; fi
done

[ "$failures" -eq 0 ] && exit 0
printf '%s check(s) failed\n' "$failures"
exit 1
