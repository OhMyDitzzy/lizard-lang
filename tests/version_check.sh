#!/bin/sh
# Checks what `lizard --version` says about the build, against the BUILDINFO that was generated
# with it (generated/BUILDINFO, installed as share/doc/lizard/BUILDINFO).
#
#   tests/version_check.sh path/to/lizard path/to/BUILDINFO

LZ=${1:?usage: version_check.sh path/to/lizard path/to/BUILDINFO}
INFO=${2:?usage: version_check.sh path/to/lizard path/to/BUILDINFO}
failures=0

fail() {
    failures=$((failures + 1))
    printf 'FAIL %s\n' "$*"
}
ok() { printf 'ok   %s\n' "$*"; }

info() { sed -n "s/^$1=//p" "$INFO" | head -1; }

out=$("$LZ" --version) || { echo "FAIL --version exited with an error"; exit 1; }
first=$(printf '%s\n' "$out" | sed -n 1p)

# the first line is the version alone (tests/skink_check.sh and scripts depend on it)
case "$first" in
    *[!0-9A-Za-z.+-]*|"") fail "first line is not a bare version: '$first'" ;;
    *) ok "first line is a version: $first" ;;
esac
[ "$first" = "$(info version)" ] && ok "matches BUILDINFO" || fail "'$first' differs from BUILDINFO version '$(info version)'"

for key in build commit channel target features; do
    printf '%s\n' "$out" | grep -q "^$key " && ok "has a '$key' line" || fail "no '$key' line"
done

kind=$(info kind)
case "$kind" in
    release|snapshot|dirty|source) ok "kind is $kind" ;;
    *) fail "unknown kind '$kind'" ;;
esac
printf '%s\n' "$out" | grep -q "^build    $kind " && ok "build line shows the kind" || fail "build line does not show kind $kind"

# kind and version string must agree
case "$kind" in
    release) case "$first" in *[+-]*) fail "a release version has no suffix: $first" ;; *) ok "release has a plain version" ;; esac ;;
    snapshot) case "$first" in *-dev.*+g*) ok "snapshot shows -dev.N+gSHA" ;; *) fail "snapshot version looks wrong: $first" ;; esac ;;
    dirty) case "$first" in *+g*.dirty.????????) ok "dirty carries a hash of the changes" ;; *) fail "dirty version looks wrong: $first" ;; esac ;;
    source) case "$first" in *+source|*+g*) ok "source version marked" ;; *) fail "source version looks wrong: $first" ;; esac ;;
esac

# the channel and the signature claim must agree
case "$(info channel)" in
    local)
        printf '%s\n' "$out" | grep -q '^channel  local (unsigned)' && ok "local build says unsigned" || fail "local channel line wrong"
        [ "$(info signed)" = 0 ] && [ -z "$(info signer)" ] && ok "local build names no signer" || fail "local build claims a signer" ;;
    official)
        printf '%s\n' "$out" | grep -q '^channel  official (signed, key ' && ok "official build names its key" || fail "official channel line wrong"
        [ "$kind" != dirty ] && ok "official build is not dirty" || fail "an official build must not be dirty" ;;
    *) fail "unknown channel '$(info channel)'" ;;
esac

if [ "$failures" -ne 0 ]; then
    printf '%s FAILURE(S)\n' "$failures"
    exit 1
fi
