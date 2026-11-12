#!/bin/sh
# Builds Lizard with nothing but a POSIX sh, awk and a C compiler (and its ar): no make, no
# autoconf, no cmake. For Termux, Alpine and other small machines. It reads the same file list
# (mk/sources.am) and test list (tests/tests.list) as the Autotools build, and the same
# tools/buildinfo.sh, so `lizard --version` is identical.
#
#   sh tools/build-plain.sh [build|check|install|clean] [--builddir=DIR] [--jobs=N] [--prefix=DIR]
#       [--debug] [--no-jit] [--lto] [--sanitizers] [--poison] [--static] [--werror]
#       [--official --signing-key=FPR] [CC=clang] [CFLAGS=...] [LDFLAGS=...]
#
# See docs/building/plain.md.

set -u

here=$(cd "$(dirname "$0")" && pwd)
src=$(cd "$here/.." && pwd)
version=0.1.0

command=build
builddir=$PWD/build-plain
jobs=
prefix=/usr/local
debug=no; jit=auto; lto=no; sanitizers=no; poison=no; static=no; werror=no
official=no; signing_key=
user_cflags=; user_cflags_set=no; user_ldflags=
CC=${CC:-cc}
AR=${AR:-ar}

die() { printf 'build-plain: error: %s\n' "$*" >&2; exit 1; }
say() { printf '%s\n' "$*" >&2; }
have() { command -v "$1" >/dev/null 2>&1; }

usage() {
    sed -n '2,/^# See docs/p' "$0" | sed 's/^# \{0,1\}//' | sed '$d'
}

# ---- arguments -----------------------------------------------------------------------------
cmd_seen=no
for arg in "$@"; do
    case $arg in
        build|check|install|clean) command=$arg; cmd_seen=yes ;;
        --builddir=*) builddir=${arg#*=}; case $builddir in /*) ;; *) builddir=$PWD/$builddir ;; esac ;;
        --jobs=*) jobs=${arg#*=} ;;
        -j[0-9]*) jobs=${arg#-j} ;;
        --prefix=*) prefix=${arg#*=} ;;
        --debug) debug=yes ;;
        --no-jit) jit=no ;;
        --jit) jit=yes ;;
        --lto) lto=yes ;;
        --sanitizers) sanitizers=yes ;;
        --poison) poison=yes ;;
        --static) static=yes ;;
        --werror) werror=yes ;;
        --official) official=yes ;;
        --signing-key=*) signing_key=${arg#*=} ;;
        CC=*) CC=${arg#*=} ;;
        AR=*) AR=${arg#*=} ;;
        CFLAGS=*) user_cflags=${arg#*=}; user_cflags_set=yes ;;
        LDFLAGS=*) user_ldflags=${arg#*=} ;;
        -h|--help) usage; exit 0 ;;
        *) die "unknown argument: $arg (try --help)" ;;
    esac
done
[ "$static" = yes ] && [ "$sanitizers" = yes ] && die "--sanitizers and --static cannot be combined"
if [ "$official" = yes ] && [ -z "$signing_key" ]; then die "--official needs --signing-key=FPR"; fi
if [ -n "$jobs" ]; then case $jobs in ''|*[!0-9]*|0) die "bad job count: $jobs" ;; esac; fi

if [ "$command" = clean ]; then
    [ -f "$builddir/.lizard-plain-build" ] || { say "nothing to clean: $builddir is not a build-plain directory"; exit 0; }
    rm -rf "$builddir"; say "removed $builddir"; exit 0
fi

have "${CC%% *}" || die "no C compiler: '$CC' not found (set CC=)"
have awk || die "awk is needed"
have "$AR" || die "'$AR' not found (the compiler's archiver is needed; set AR=)"

if [ -z "$jobs" ]; then
    jobs=$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || getconf _NPROCESSORS_ONLN 2>/dev/null || echo 1)
    case $jobs in ''|*[!0-9]*) jobs=1 ;; esac
fi

# ---- the lists (same awk as tools/check-sync.sh) ------------------------------------------------
list_of() { # variable of mk/sources.am -> one item per line
    awk -v v="$1" '
        /^[ \t]*#/ { next }
        { s = $0; cont = (s ~ /\\[ \t]*$/); sub(/\\[ \t]*$/, "", s); buf = buf " " s
          if (!cont) { if (match(buf, /^ *[A-Za-z0-9_]+ *=/)) {
                name = substr(buf, 1, RLENGTH); sub(/ *=$/, "", name); gsub(/ /, "", name)
                if (name == v) { rest = substr(buf, RLENGTH + 1); n = split(rest, a, /[ \t]+/)
                    for (i = 1; i <= n; i++) if (a[i] != "") print a[i] } }
            buf = "" } }' "$src/mk/sources.am"
}
tests_list() { awk '$0 !~ /^[ \t]*#/ && NF' "$src/tests/tests.list"; }

# ---- platform: JIT exists for x86_64 and aarch64 on Linux and macOS --------------------------------
arch=$(uname -m 2>/dev/null); os=$(uname -s 2>/dev/null)
jit_platform=no
case "$arch:$os" in
    x86_64:Linux|x86_64:Darwin|aarch64:Linux|aarch64:Darwin|arm64:Linux|arm64:Darwin) jit_platform=yes ;;
esac
case $jit in
    auto|yes) jit_on=$jit_platform
        [ "$jit" = yes ] && [ "$jit_on" = no ] && say "warning: no JIT backend for $arch-$os: building with -DLZ_NO_JIT" ;;
    no) jit_on=no ;;
esac

# ---- flags -----------------------------------------------------------------------------------------
config=custom
if [ "$user_cflags_set" = yes ]; then
    opt=$user_cflags
elif [ "$debug" = yes ]; then opt="-g -O0"; config=Debug
else opt="-O2 -DNDEBUG"; config=Release
fi

try_flag() { # does $CC accept this flag? (-Werror: clang only warns about unknown ones)
    probe=$builddir/.probe.c
    printf 'int main(void) { return 0; }\n' >"$probe"
    $CC -std=c11 -Werror "$1" -c "$probe" -o "$builddir/.probe.o" >/dev/null 2>&1
    rc=$?
    rm -f "$probe" "$builddir/.probe.o"
    return $rc
}

mkdir -p "$builddir" || die "cannot create $builddir"
: >"$builddir/.lizard-plain-build"
$CC -std=c11 -E -x c /dev/null >/dev/null 2>&1 || $CC -std=c11 -E - </dev/null >/dev/null 2>&1 \
    || die "$CC does not accept -std=c11: a C11 compiler is needed"

warn_flags=
for f in -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wstrict-prototypes -Wmissing-prototypes -Wformat=2; do
    try_flag $f && warn_flags="$warn_flags $f"
done
[ "$werror" = yes ] && warn_flags="$warn_flags -Werror"

cflags="-std=c11$warn_flags $opt"
ldflags=$user_ldflags
cppflags="-I$src/include -I$src/builtin/include -I$src/builtin -I$builddir/generated"
[ "$poison" = yes ] && cppflags="$cppflags -DLZ_POISON"
if [ "$sanitizers" = yes ]; then
    san="-fsanitize=address,undefined"
    try_flag "$san" || die "--sanitizers: $CC cannot build with ASan and UBSan here"
    cflags="$cflags $san -fno-omit-frame-pointer"; ldflags="$ldflags $san"
fi
lto_on=no
if [ "$lto" = yes ]; then
    if try_flag -flto; then lto_on=yes; cflags="$cflags -flto"; ldflags="$ldflags -flto"
    else say "warning: LTO requested but not supported by $CC: building without it"; fi
fi
engine_cppflags=$cppflags
[ "$jit_on" = no ] && engine_cppflags="$engine_cppflags -DLZ_NO_JIT"
static_ldflags=
if [ "$static" = yes ]; then
    if [ "$os" = Darwin ]; then say "warning: macOS has no static executables: --static ignored"; static=no
    else static_ldflags=-static; fi
fi

# the -lm of the Autotools build (AC_SEARCH_LIBS): needed on most systems, harmless on others
libs=-lm

# ---- build information -----------------------------------------------------------------------------
channel=local; [ "$official" = yes ] && channel=official
sh "$here/buildinfo.sh" --srcdir="$src" --builddir="$builddir" --version="$version" --channel="$channel" \
    --signing-key="$signing_key" --config="$config" --cc="$CC" --jit="$jit_on" --lto="$lto_on" \
    --sanitizers="$sanitizers" --static="$static" || exit 1

# ---- compiling (parallel, incremental) -------------------------------------------------------------
objdir=$builddir/obj
mkdir -p "$objdir" "$builddir/.status"
rm -f "$builddir/.status/"* 2>/dev/null

# An object is stale when its source, or a header the compiler saw it include (the .d file that
# -MMD -MP writes next to it), is newer. No .d file means the object is built again.
stale() { # object source
    [ -f "$1" ] || return 0
    [ "$2" -nt "$1" ] 2>/dev/null && return 0
    [ -f "$1.d" ] || return 0
    for dep in $(sed -e 's/^[^:]*://' -e 's/\\$//' "$1.d" | tr -s ' \n' '\n\n'); do
        [ -f "$dep" ] || return 0 # a header that is gone: the object can't be trusted
        [ "$dep" -nt "$1" ] && return 0
    done
    return 1
}

running=0
batch_wait() { wait; running=0; }
nfail() { n=0; for f in "$builddir/.status/"*; do [ -e "$f" ] && n=$((n + 1)); done; echo $n; }

compile() { # object source extra-flags...
    obj=$1; csrc=$2; shift 2
    mkdir -p "$(dirname "$obj")"
    stale "$obj" "$csrc" || return 0
    say "  CC  ${csrc#"$src"/}"
    (
        # shellcheck disable=SC2086
        if ! $CC $cflags "$@" -MMD -MP -MF "$obj.d" -c "$csrc" -o "$obj.tmp"; then
            : >"$builddir/.status/$(basename "$obj").fail"; rm -f "$obj.tmp"
        else mv -f "$obj.tmp" "$obj"; fi
    ) &
    running=$((running + 1))
    [ "$running" -lt "$jobs" ] || batch_wait
}

objname() { printf '%s' "$1" | sed 's|/|_|g; s|\.c$|.o|'; }

build_lib() { # name cppflags
    lib=$1; cpp=$2
    objs=
    for f in $(list_of "lizard_${lib}_SOURCES"); do
        o=$objdir/$lib/$(objname "$f"); objs="$objs $o"
        # shellcheck disable=SC2086
        compile "$o" "$src/$f" $cpp
    done
    batch_wait
    [ "$(nfail)" -eq 0 ] || die "compilation failed"
    if [ ! -f "$builddir/liblizard_$lib.a" ] || [ -n "$(find $objs -newer "$builddir/liblizard_$lib.a" 2>/dev/null | head -1)" ]; then
        rm -f "$builddir/liblizard_$lib.a"
        # shellcheck disable=SC2086
        "$AR" rc "$builddir/liblizard_$lib.a" $objs || die "ar failed for $lib"
        if have ranlib; then ranlib "$builddir/liblizard_$lib.a"; fi
    fi
}

link() { # output ldflags objects/libs...
    out=$1; extra=$2; shift 2
    if [ -f "$out" ]; then
        newer=
        for dep in "$@"; do case $dep in -*) ;; *) [ "$dep" -nt "$out" ] && newer=yes ;; esac; done
        [ -z "$newer" ] && return 0
    fi
    # shellcheck disable=SC2086
    $CC $cflags $ldflags $extra -o "$out" "$@" $libs || die "link failed: $out"
}

b=$builddir
say "==> building ($config, $CC, jit=$jit_on)"
build_lib core "$cppflags"
build_lib frontend "$cppflags"
build_lib engine "$engine_cppflags"
build_lib builtin "$cppflags"
build_lib utils "$cppflags"

main=$(list_of lizard_SOURCES)
compile "$objdir/cli/$(objname "$main")" "$src/$main" $cppflags
batch_wait
[ "$(nfail)" -eq 0 ] || die "compilation failed"
link "$b/lizard" "$static_ldflags" "$objdir/cli/$(objname "$main")" \
    "$b/liblizard_engine.a" "$b/liblizard_frontend.a" "$b/liblizard_builtin.a" "$b/liblizard_core.a" "$b/liblizard_utils.a"

# ---- tests -------------------------------------------------------------------------------------------
# (no pipelines around compile: background jobs must belong to this shell, so that `wait` sees them)
test_programs() { # -> "program build" per line, without duplicates
    tests_list | awk '$3 == "plain" || $3 == "ge" || $3 == "engine" || $3 == "utils" { print $2, $3 }' | sort -u
}
build_tests() {
    test_programs >"$b/.test-programs"
    while read -r prog build; do
        tsrc=${prog%_ge}
        o=$objdir/tests/$prog.o
        case $build in
            ge) compile "$o" "$src/tests/$tsrc.c" $cppflags -include "$src/tests/engine_redirect.h" ;;
            *) compile "$o" "$src/tests/$tsrc.c" $cppflags ;;
        esac
    done <"$b/.test-programs"
    batch_wait
}
link_tests() {
    while read -r prog build; do
        o=$objdir/tests/$prog.o
        case $build in
            plain) link "$b/$prog" "" "$o" "$b/liblizard_frontend.a" "$b/liblizard_builtin.a" "$b/liblizard_core.a" ;;
            utils) link "$b/$prog" "" "$o" "$b/liblizard_utils.a" ;;
            *) link "$b/$prog" "" "$o" "$b/liblizard_engine.a" "$b/liblizard_frontend.a" "$b/liblizard_builtin.a" "$b/liblizard_core.a" ;;
        esac
    done <"$b/.test-programs"
}

if [ "$command" = check ]; then
    say "==> building the tests"
    build_tests
    [ "$(nfail)" -eq 0 ] || die "compilation of the tests failed"
    link_tests

    say "==> running the tests ($jobs at a time)"
    logs=$b/test-logs; rm -rf "$logs" "$b/.res"; mkdir -p "$logs" "$b/.res"
    tests_list | awk '{print $1}' >"$b/check-tests.list"
    running=0
    while read -r t; do
        (
            cd "$b" && sh "$src/tests/run-test.sh" "$t" >"$logs/$t.log" 2>&1
            echo $? >"$b/.res/$t"
        ) &
        running=$((running + 1))
        [ "$running" -lt "$jobs" ] || { wait; running=0; }
    done <"$b/check-tests.list"
    wait
    : >"$b/check-results.txt"
    while read -r t; do
        rc=$(cat "$b/.res/$t" 2>/dev/null || echo 99)
        case $rc in
            0) echo "PASS: $t" ;;
            77) echo "SKIP: $t" ;;
            *) echo "FAIL: $t (exit $rc)" ;;
        esac >>"$b/check-results.txt"
    done <"$b/check-tests.list"
    grep -v '^PASS' "$b/check-results.txt" >&2
    pass=$(grep -c '^PASS' "$b/check-results.txt"); skip=$(grep -c '^SKIP' "$b/check-results.txt")
    fail=$(grep -c '^FAIL' "$b/check-results.txt")
    for t in $(sed -n 's/^FAIL: \([^ ]*\).*/\1/p' "$b/check-results.txt"); do
        say "---- $t"; tail -n 20 "$logs/$t.log" >&2
    done
    say "=> $((pass + skip + fail)) tests: $pass passed, $skip skipped, $fail failed (logs: $logs)"
    [ "$fail" -eq 0 ] || exit 1
    exit 0
fi

# ---- install: the same layout as `make install` -------------------------------------------------------
if [ "$command" = install ]; then
    dest=${DESTDIR:-}
    inst() { # mode source destination-dir [name]
        mkdir -p "$dest$3" || die "cannot create $dest$3"
        cp "$2" "$dest$3/${4:-$(basename "$2")}" && chmod "$1" "$dest$3/${4:-$(basename "$2")}"
    }
    inst 755 "$b/lizard" "$prefix/bin"
    docdir=$prefix/share/doc/lizard
    for f in $(list_of lizard_docs); do
        inst 644 "$src/$f" "$docdir/$(dirname "$f" | sed 's|^\.$||')"
    done
    inst 644 "$b/generated/BUILDINFO" "$docdir"
    for f in $(list_of lizard_examples); do inst 644 "$src/$f" "$prefix/share/lizard/examples"; done
    say "installed under $dest$prefix"
    exit 0
fi

say "built $b/lizard"
