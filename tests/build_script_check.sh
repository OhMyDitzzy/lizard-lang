#!/usr/bin/env bash
# Smoke tests of build.sh that need no compiler run: help, errors, platform detection, target names.
#
#   tests/build_script_check.sh path/to/build.sh

BUILD=${1:?usage: build_script_check.sh path/to/build.sh}
failures=0
fail() { failures=$((failures + 1)); printf 'FAIL %s\n' "$*"; }
ok() { printf 'ok   %s\n' "$*"; }

"$BUILD" --help | grep -q '^Usage: ./build.sh' && ok "--help prints the usage" || fail "--help"

out=$("$BUILD" --bogus 2>&1); rc=$?
[ "$rc" -ne 0 ] && printf '%s' "$out" | grep -q 'unknown option' && ok "an unknown option is an error" || fail "unknown option accepted"

out=$("$BUILD" build --enable-static --enable-sanitizers 2>&1); rc=$?
[ "$rc" -ne 0 ] && printf '%s' "$out" | grep -q 'cannot be combined' && ok "static + sanitizers refused" || fail "static + sanitizers accepted"

out=$("$BUILD" build --build-type=Fast 2>&1); rc=$?
[ "$rc" -ne 0 ] && ok "a bad build type is refused" || fail "bad build type accepted"

info=$("$BUILD" detect --porcelain 2>&1)
host=$(printf '%s\n' "$info" | sed -n 's/^host=//p')
arch=$(printf '%s\n' "$info" | sed -n 's/^arch=//p')
os=$(printf '%s\n' "$info" | sed -n 's/^os=//p')
case "$host" in
    "$arch-$os"*) ok "detected $host" ;;
    *) fail "detect gave host='$host' arch='$arch' os='$os'" ;;
esac
[ "$arch" = "$(uname -m | sed 's/arm64/aarch64/;s/amd64/x86_64/')" ] && ok "arch agrees with uname" || fail "arch '$arch' vs uname $(uname -m)"

# target names: every spelling of a triplet maps to one canonical name (parse_target is internal,
# so source the helper functions only)
eval "$(sed -n '/^lower()/,/^HOST_ARCH=/p' "$BUILD" | sed '$d')"
die() { echo "die: $*" >&2; return 1; }
check_target() { # spelling canonical gnu
    parse_target "$1" 2>/dev/null
    [ "$T_NAME" = "$2" ] && [ "$T_GNU" = "$3" ] && ok "$1 -> $2" || fail "$1 -> '$T_NAME' / '$T_GNU', expected $2 / $3"
}
check_target x86_64-pc-linux-gnu x86_64-linux-gnu x86_64-linux-gnu
check_target arm64-linux-musl aarch64-linux-musl aarch64-linux-musl
check_target arm-linux-gnueabihf armv7-linux-gnueabihf arm-linux-gnueabihf
check_target arm64-apple-darwin aarch64-darwin aarch64-apple-darwin
check_target x86_64-w64-mingw32 x86_64-windows-mingw x86_64-w64-mingw32

[ "$failures" -eq 0 ] || { printf '%s FAILURE(S)\n' "$failures"; exit 1; }
