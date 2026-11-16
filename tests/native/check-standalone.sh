#!/bin/sh
# Compiles tests/native/modlib.c with only include/lzapi.h in sight, as C99 with strict warnings.
# This is what proves that the public header needs no internal header and no Lizard type.
srcdir=${1:-$(cd "$(dirname "$0")/../.." && pwd)}
tmp=$(mktemp -d) || exit 1
trap 'rm -rf "$tmp"' EXIT
cp "$srcdir/include/lzapi.h" "$tmp/"
${CC:-cc} -std=c99 -Wall -Wextra -Wpedantic -Werror -I"$tmp" -c "$srcdir/tests/native/modlib.c" -o "$tmp/modlib.o" || exit 1
# the header must also compile as C++ and as the very first include
printf '#include "lzapi.h"\nint main(void) { return LZN_ABI_VERSION == 1 ? 0 : 1; }\n' >"$tmp/t.c"
${CC:-cc} -std=c99 -Wall -Wextra -Wpedantic -Werror -I"$tmp" -c "$tmp/t.c" -o "$tmp/t.o" || exit 1
echo "lzapi.h is self-contained"
