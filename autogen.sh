#!/bin/sh
# Generates ./configure and the Makefile.in files from a git checkout.
# Needs autoconf (2.69+) and automake (1.16+). A release tarball (make dist) already has them.
set -e
cd "$(dirname "$0")"
sh tools/gen-tests-am.sh >mk/tests.am.new
if cmp -s mk/tests.am.new mk/tests.am 2>/dev/null; then rm -f mk/tests.am.new; else mv mk/tests.am.new mk/tests.am; fi
exec autoreconf -fi "$@"
