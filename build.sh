#!/usr/bin/env bash
# build.sh - detects the system, configures Lizard for it, builds it, and packages it.
#
# A GNU-style driver around CMake: --prefix=, --host=, --enable-/--disable-, VAR=value
# (CC=clang ./build.sh build). It needs bash (3.2 or newer), cmake 3.16+, a C11 compiler and,
# for packages, tar and gzip or xz. git is optional but gives the build its commit and kind.
#
#   ./build.sh detect                       what this machine is, and what it can build for
#   ./build.sh build                        configure and build for this machine (the default)
#   ./build.sh check                        build, then run the tests
#   ./build.sh package                      build and write build/../dist/lizard-<version>-<target>.tar.gz
#   ./build.sh package --targets=all        one tarball per ABI and platform that has a toolchain
#   ./build.sh package --official           signed release: clean tree, signed tarballs and SHA256SUMS
#   ./build.sh verify [dir]                 check checksums and signatures of a dist directory
#   ./build.sh sourcetar                    source tarball that remembers its commit (see docs/BUILDING.md)
#   ./build.sh clean
#
# ./build.sh --help lists every option.

set -euo pipefail

SCRIPT_VERSION=1
SRC_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)

if [ -t 2 ] && [ -z "${NO_COLOR:-}" ]; then
    C_BOLD=$'\033[1m'; C_RED=$'\033[31m'; C_YEL=$'\033[33m'; C_GRN=$'\033[32m'; C_OFF=$'\033[0m'
else
    C_BOLD=''; C_RED=''; C_YEL=''; C_GRN=''; C_OFF=''
fi
QUIET=0
VERBOSE=0
say()  { [ "$QUIET" -eq 1 ] || printf '%s\n' "$*" >&2; }
step() { [ "$QUIET" -eq 1 ] || printf '%s==>%s %s%s%s\n' "$C_GRN" "$C_OFF" "$C_BOLD" "$*" "$C_OFF" >&2; }
warn() { printf '%swarning:%s %s\n' "$C_YEL" "$C_OFF" "$*" >&2; }
die()  { printf '%serror:%s %s\n' "$C_RED" "$C_OFF" "$*" >&2; exit 1; }

COMMAND=""
PREFIX=/usr/local
BUILD_TYPE=Release
BUILD_ROOT="$SRC_DIR/build"
OUT_DIR="$SRC_DIR/dist"
GENERATOR=""
JOBS=""
TARGETS=""          # empty: this machine
ENABLE_JIT=auto     # auto | yes | no
ENABLE_LTO=no
ENABLE_SANITIZERS=no
ENABLE_STATIC=no
ENABLE_TESTS=yes
WERROR=no
CHANNEL=local
SIGN_KEY="${LIZARD_SIGNING_KEY:-}"
COMPRESS=gz
STRICT=no
RUN_CHECK=no
CLEAN_FIRST=no
SYSROOT=""
KEEP_GOING=no
EXPECT_KEY=""
PORCELAIN=no
ARGS_REST=()

usage() {
    cat <<'EOF'
Usage: ./build.sh [COMMAND] [OPTIONS] [VAR=value ...]

Commands
  detect       Show the build machine, the compiler, the tools, and which targets can be built.
  configure    Run CMake for each target.
  build        Configure and build (default).
  check        Build, then run the test suite (this machine's target only).
  package      Build, install into a staging tree and write dist/lizard-<version>-<target>.tar.*
               plus SHA256SUMS (and, with --official, detached .asc signatures).
  verify [DIR] Check SHA256SUMS and every .asc signature in DIR (default: dist).
  sourcetar    Write dist/lizard-<version>-src.tar.gz from the committed tree, with a .buildinfo
               so that a build without git still knows its commit.
  clean        Remove the build directory.

Targets and platform
  --targets=LIST     Comma-separated targets: native (default), all, or names such as
                     x86_64-linux-gnu, aarch64-linux-gnu, x86_64-linux-musl, aarch64-linux-musl,
                     armv7-linux-gnueabihf, riscv64-linux-gnu, x86_64-darwin, aarch64-darwin,
                     x86_64-freebsd. GNU triplets (x86_64-pc-linux-gnu) are accepted too.
                     "all" is the Linux matrix above. Windows (x86_64-windows-mingw) is accepted but
                     untested: the JIT is off there.
  --host=TRIPLET     Same as --targets with one target (the GNU configure spelling).
  --sysroot=DIR      Sysroot for a clang or zig cross compiler.
  --strict           A target without a toolchain is an error, not a skip.
  --keep-going       Continue with the other targets when one fails.

Configuration
  --prefix=DIR       Install prefix for `cmake --install` (default /usr/local). Packages are relocatable.
  --build-type=T     Release (default), Debug, RelWithDebInfo, MinSizeRel. Also --debug, --release.
  --enable-jit / --disable-jit      The JIT (default: on where it exists: x86-64 and AArch64, Linux and macOS).
  --enable-lto                      Link-time optimization.
  --enable-static                   Static executable (the usual choice for musl packages).
  --enable-sanitizers               ASan and UBSan (not with --enable-static).
  --disable-tests                   Do not build the tests (cross targets never build them).
  --enable-werror                   Warnings are errors.
  CC=... CFLAGS=... LDFLAGS=...     Compiler and flags for this machine's target.

Build identity
  --local            An unsigned local build (default). Shows as "local (unsigned)".
  --official         A signed build: the tree must be clean, a GPG key is needed, the packages and
                     SHA256SUMS get detached signatures. Shows as "official (signed, key ...)".
  --sign-key=KEY     GPG key to sign with (default: $LIZARD_SIGNING_KEY, else your first secret key).
  --expect-key=FPR   For `verify`: require that this fingerprint made the signatures.

Output
  --builddir=DIR     Build trees (default ./build; one subdirectory per target and build type).
  --outdir=DIR       Where packages go (default ./dist).
  --compress=gz|xz   Package compression (default gz).
  --check            With `package`: run the tests first (this machine's target only).
  --clean-first      Start from empty build directories.
  --generator=NAME   CMake generator (default: Ninja if installed, else Unix Makefiles).
  -j N, --jobs=N     Parallel jobs (default: number of CPUs).
  -q, --quiet        Fewer messages.     -v, --verbose   Show every command.
  --porcelain        With `detect`: key=value lines for scripts.
  -h, --help         This text.          --script-version

Environment: SOURCE_DATE_EPOCH (reproducible dates), GNUPGHOME, LIZARD_SIGNING_KEY, NO_COLOR.
EOF
}

# ---------------------------------------------------------------------------- arguments
need_value() { [ -n "${2:-}" ] || die "$1 needs a value"; }

parse_args() {
    while [ $# -gt 0 ]; do
        case "$1" in
            -h|--help) usage; exit 0 ;;
            --script-version) echo "build.sh $SCRIPT_VERSION"; exit 0 ;;
            -q|--quiet) QUIET=1 ;;
            -v|--verbose) VERBOSE=1 ;;
            --porcelain) PORCELAIN=yes ;;
            --prefix=*) PREFIX=${1#*=} ;;
            --prefix) need_value "$1" "${2:-}"; PREFIX=$2; shift ;;
            --build-type=*) BUILD_TYPE=${1#*=} ;;
            --debug) BUILD_TYPE=Debug ;;
            --release) BUILD_TYPE=Release ;;
            --targets=*|--target=*|--host=*) TARGETS=${1#*=} ;;
            --sysroot=*) SYSROOT=${1#*=} ;;
            --strict) STRICT=yes ;;
            --keep-going) KEEP_GOING=yes ;;
            --enable-jit) ENABLE_JIT=yes ;;
            --disable-jit) ENABLE_JIT=no ;;
            --enable-lto) ENABLE_LTO=yes ;;
            --disable-lto) ENABLE_LTO=no ;;
            --enable-static) ENABLE_STATIC=yes ;;
            --disable-static) ENABLE_STATIC=no ;;
            --enable-sanitizers) ENABLE_SANITIZERS=yes ;;
            --disable-sanitizers) ENABLE_SANITIZERS=no ;;
            --disable-tests) ENABLE_TESTS=no ;;
            --enable-tests) ENABLE_TESTS=yes ;;
            --enable-werror) WERROR=yes ;;
            --disable-werror) WERROR=no ;;
            --local) CHANNEL=local ;;
            --official) CHANNEL=official ;;
            --sign-key=*) SIGN_KEY=${1#*=} ;;
            --expect-key=*) EXPECT_KEY=${1#*=} ;;
            --builddir=*) BUILD_ROOT=$(abs_path "${1#*=}") ;;
            --outdir=*) OUT_DIR=$(abs_path "${1#*=}") ;;
            --compress=*) COMPRESS=${1#*=} ;;
            --check) RUN_CHECK=yes ;;
            --clean-first) CLEAN_FIRST=yes ;;
            --generator=*) GENERATOR=${1#*=} ;;
            -j) need_value "$1" "${2:-}"; JOBS=$2; shift ;;
            -j[0-9]*) JOBS=${1#-j} ;;
            --jobs=*) JOBS=${1#*=} ;;
            CC=*|CFLAGS=*|LDFLAGS=*|AR=*|STRIP=*) export "${1?}" ;;
            -*) die "unknown option: $1 (try --help)" ;;
            *)
                if [ -z "$COMMAND" ]; then COMMAND=$1; else ARGS_REST+=("$1"); fi ;;
        esac
        shift
    done
    COMMAND=${COMMAND:-build}

    case "$BUILD_TYPE" in Release|Debug|RelWithDebInfo|MinSizeRel) ;; *) die "bad --build-type: $BUILD_TYPE" ;; esac
    case "$COMPRESS" in gz|xz) ;; *) die "--compress must be gz or xz" ;; esac
    case "$CHANNEL" in local|official) ;; *) die "bad channel" ;; esac
    if [ -n "$JOBS" ]; then
        case "$JOBS" in ''|*[!0-9]*) die "bad job count: $JOBS" ;; esac
    fi
    if [ "$ENABLE_STATIC" = yes ] && [ "$ENABLE_SANITIZERS" = yes ]; then
        die "--enable-static and --enable-sanitizers cannot be combined"
    fi
}

abs_path() {
    case "$1" in
        /*) printf '%s\n' "$1" ;;
        *) printf '%s\n' "$PWD/$1" ;;
    esac
}

run() {
    if [ "$VERBOSE" -eq 1 ]; then printf '+ %s\n' "$*" >&2; fi
    "$@"
}

have() { command -v "$1" >/dev/null 2>&1; }
onoff() { if [ "$1" = yes ]; then echo ON; else echo OFF; fi; }

# ---------------------------------------------------------------------------- detection
lower() { printf '%s' "$1" | tr 'A-Z' 'a-z'; }

norm_arch() {
    case "$(lower "$1")" in
        x86_64|amd64|x64) echo x86_64 ;;
        aarch64|arm64|armv8*) echo aarch64 ;;
        armv7*|armhf|arm) echo armv7 ;;
        i[3-6]86|x86) echo i686 ;;
        riscv64) echo riscv64 ;;
        ppc64le|powerpc64le) echo ppc64le ;;
        s390x) echo s390x ;;
        *) echo "$(lower "$1")" ;;
    esac
}

detect_os() {
    case "$(uname -s)" in
        Linux) echo linux ;;
        Darwin) echo darwin ;;
        FreeBSD) echo freebsd ;;
        OpenBSD) echo openbsd ;;
        NetBSD) echo netbsd ;;
        MINGW*|MSYS*|CYGWIN*) echo windows ;;
        *) echo "$(lower "$(uname -s)")" ;;
    esac
}

find_cc() {
    if [ -n "${CC:-}" ]; then
        have "${CC%% *}" || die "CC=$CC: not found"
        printf '%s\n' "$CC"
        return
    fi
    local c
    for c in cc gcc clang; do
        if have "$c"; then printf '%s\n' "$c"; return; fi
    done
    die "no C compiler found (looked for cc, gcc, clang); install one or set CC="
}

# ABI of a Linux system from the compiler's triple, else from the C library.
detect_linux_abi() {
    local triple=$1 abi=""
    case "$triple" in
        *-linux-*) abi=${triple##*linux-} ;;
    esac
    case "$abi" in
        gnu*|musl*) printf '%s\n' "$abi"; return ;;
    esac
    if have ldd && ldd --version 2>&1 | grep -qi musl; then echo musl; return; fi
    echo gnu
}

# How GNU toolchains and zig spell an architecture.
compiler_arch() {
    case "$1" in
        armv7) echo arm ;;
        ppc64le) echo powerpc64le ;;
        *) echo "$1" ;;
    esac
}

# Parses a target name into T_ARCH T_OS T_ABI T_NAME T_GNU (the GNU triplet used to find a
# cross compiler) and T_ZIG (the spelling zig cc wants).
parse_target() {
    local spec arch os abi=""
    spec=$(lower "$1")
    arch=$(norm_arch "${spec%%-*}")
    case "$spec" in
        *darwin*|*apple*|*macos*) os=darwin ;;
        *freebsd*) os=freebsd ;;
        *openbsd*) os=openbsd ;;
        *netbsd*) os=netbsd ;;
        *mingw*|*windows*|*w64*) os=windows; abi=mingw ;;
        *linux*)
            os=linux
            case "$spec" in
                *musleabihf) abi=musleabihf ;;
                *musl*) abi=musl ;;
                *gnueabihf) abi=gnueabihf ;;
                *gnueabi) abi=gnueabi ;;
                *gnu*) abi=gnu ;;
                *) abi=gnu ;;
            esac ;;
        *) die "unrecognized target: $1 (expected something like x86_64-linux-gnu)" ;;
    esac
    if [ "$arch" = armv7 ] && [ "$os" = linux ]; then
        case "$abi" in gnu) abi=gnueabihf ;; musl) abi=musleabihf ;; esac
    fi
    T_ARCH=$arch
    T_OS=$os
    T_ABI=$abi
    case "$os" in
        linux) T_NAME="$arch-linux-$abi" ;;
        windows) T_NAME="$arch-windows-mingw" ;;
        *) T_NAME="$arch-$os" ;;
    esac
    case "$os" in
        linux) T_GNU="$(compiler_arch "$arch")-linux-$abi" ;;
        windows) T_GNU="$arch-w64-mingw32" ;;
        darwin) T_GNU="$arch-apple-darwin" ;;
        *) T_GNU="$arch-unknown-$os" ;;
    esac
    case "$os" in
        linux) T_ZIG="$(compiler_arch "$arch")-linux-$abi" ;;
        windows) T_ZIG="$arch-windows-gnu" ;;
        darwin) T_ZIG="$arch-macos" ;;
        *) T_ZIG="$arch-$os" ;;
    esac
}

HOST_ARCH=""; HOST_OS=""; HOST_ABI=""; HOST_NAME=""; HOST_CC=""; HOST_TRIPLE=""; HOST_CC_VERSION=""

detect_host() {
    HOST_OS=$(detect_os)
    HOST_ARCH=$(norm_arch "$(uname -m)")
    HOST_CC=$(find_cc)
    HOST_TRIPLE=$($HOST_CC -dumpmachine 2>/dev/null || true)
    HOST_CC_VERSION=$($HOST_CC --version 2>/dev/null | head -1 || true)
    HOST_ABI=""
    case "$HOST_OS" in
        linux) HOST_ABI=$(detect_linux_abi "$HOST_TRIPLE") ;;
        windows) HOST_ABI=mingw ;;
    esac
    # An ARM kernel can run a 32-bit userland, and an x86-64 kernel a 32-bit one: trust the compiler.
    if [ -n "$HOST_TRIPLE" ]; then
        local carch
        carch=$(norm_arch "${HOST_TRIPLE%%-*}")
        case "$carch" in x86_64|aarch64|armv7|i686|riscv64|ppc64le|s390x) HOST_ARCH=$carch ;; esac
    fi
    case "$HOST_OS" in
        linux) HOST_NAME="$HOST_ARCH-linux-$HOST_ABI" ;;
        windows) HOST_NAME="$HOST_ARCH-windows-mingw" ;;
        *) HOST_NAME="$HOST_ARCH-$HOST_OS" ;;
    esac
}

cpu_count() {
    if [ -n "$JOBS" ]; then echo "$JOBS"; return; fi
    local n=""
    if have nproc; then n=$(nproc 2>/dev/null || true); fi
    [ -n "$n" ] || n=$(getconf _NPROCESSORS_ONLN 2>/dev/null || true)
    [ -n "$n" ] || n=$(sysctl -n hw.ncpu 2>/dev/null || true)
    echo "${n:-1}"
}

pick_generator() {
    if [ -n "$GENERATOR" ]; then echo "$GENERATOR"; return; fi
    if have ninja; then echo Ninja; else echo "Unix Makefiles"; fi
}

# Does the JIT exist for this target? Mirrors src/engine/ge_jit.h.
jit_supported() {
    case "$1:$2" in
        x86_64:linux|x86_64:darwin|aarch64:linux|aarch64:darwin) return 0 ;;
    esac
    return 1
}

# ---------------------------------------------------------------------------- git
GIT_OK=no
detect_git() {
    GIT_OK=no
    if ! have git; then return 0; fi
    if [ "$(git -C "$SRC_DIR" rev-parse --show-prefix 2>/dev/null || echo x)" = "" ]; then GIT_OK=yes; fi
}

tree_is_dirty() {
    if [ "$GIT_OK" != yes ]; then return 1; fi
    # Build directories under the source tree do not count (same rule as cmake/GenerateBuildInfo.cmake).
    local line dir
    while IFS= read -r line; do
        [ -n "$line" ] || continue
        case "$line" in
            '?? '*/)
                dir=${line#'?? '}
                if [ -e "$SRC_DIR/${dir}CMakeCache.txt" ] || [ -e "$SRC_DIR/${dir}CMakeFiles" ]; then continue; fi ;;
        esac
        return 0
    done < <(git -C "$SRC_DIR" status --porcelain --untracked-files=normal)
    return 1
}

source_date_epoch() {
    if [ -n "${SOURCE_DATE_EPOCH:-}" ]; then echo "$SOURCE_DATE_EPOCH"; return; fi
    if [ "$GIT_OK" = yes ]; then
        local t
        t=$(git -C "$SRC_DIR" log -1 --format=%ct 2>/dev/null || true)
        if [ -n "$t" ]; then echo "$t"; return; fi
    fi
    date +%s
}

# ---------------------------------------------------------------------------- targets
# Toolchain lookup for a target other than this machine's. Sets T_CC (a compiler command or a
# generated wrapper script) and T_CC_HOW, or returns 1.
find_cross_cc() {
    local bdir=$1 c
    T_CC=""; T_CC_HOW=""
    if [ -n "${CC:-}" ] && [ "$CROSS_CC_FROM_ENV" = yes ]; then
        have "${CC%% *}" || die "CC=$CC: not found"
        T_CC=$CC; T_CC_HOW="CC=$CC"; return 0
    fi
    # darwin to darwin: the same compiler with -arch (CMAKE_OSX_ARCHITECTURES), nothing to find.
    if [ "$T_OS" = darwin ] && [ "$HOST_OS" = darwin ]; then
        T_CC=$HOST_CC; T_CC_HOW="$HOST_CC -arch $T_ARCH"; return 0
    fi
    for c in "$T_GNU-gcc" "$T_GNU-cc" "$T_ARCH-unknown-$T_OS-$T_ABI-gcc"; do
        if have "$c"; then T_CC=$c; T_CC_HOW=$c; return 0; fi
    done
    # Wrapper scripts: CMake wants one executable, and these need arguments.
    mkdir -p "$bdir"
    if have zig; then
        T_CC="$bdir/cc-wrapper.sh"
        printf '#!/bin/sh\nexec zig cc -target %s "$@"\n' "$T_ZIG" >"$T_CC"
        chmod +x "$T_CC"; T_CC_HOW="zig cc -target $T_ZIG"; return 0
    fi
    if have clang && { [ -n "$SYSROOT" ] || [ "$T_OS" = freebsd ]; }; then
        T_CC="$bdir/cc-wrapper.sh"
        if [ -n "$SYSROOT" ]; then
            printf '#!/bin/sh\nexec clang --target=%s --sysroot=%s "$@"\n' "$T_GNU" "$SYSROOT" >"$T_CC"
        else
            printf '#!/bin/sh\nexec clang --target=%s "$@"\n' "$T_GNU" >"$T_CC"
        fi
        chmod +x "$T_CC"; T_CC_HOW="clang --target=$T_GNU"; return 0
    fi
    return 1
}

CROSS_CC_FROM_ENV=no
DEFAULT_MATRIX="x86_64-linux-gnu aarch64-linux-gnu x86_64-linux-musl aarch64-linux-musl armv7-linux-gnueabihf riscv64-linux-gnu"

expand_targets() {
    local list=${TARGETS:-native} item out="" parts
    TARGET_LIST=()
    IFS=',' read -r -a parts <<<"$list"
    for item in "${parts[@]}"; do
        case "$item" in
            native|"") TARGET_LIST+=("$HOST_NAME") ;;
            all) for out in $DEFAULT_MATRIX; do TARGET_LIST+=("$out"); done ;;
            *) parse_target "$item"; TARGET_LIST+=("$T_NAME") ;;
        esac
    done
    # a list without duplicates, in order
    local seen=" " unique=() t
    for t in "${TARGET_LIST[@]}"; do
        case "$seen" in *" $t "*) continue ;; esac
        seen="$seen$t "
        unique+=("$t")
    done
    TARGET_LIST=("${unique[@]}")
    if [ -n "${CC:-}" ] && [ "${#TARGET_LIST[@]}" -eq 1 ] && [ "${TARGET_LIST[0]}" != "$HOST_NAME" ]; then
        CROSS_CC_FROM_ENV=yes
    fi
}

target_bdir() { printf '%s/%s-%s\n' "$BUILD_ROOT" "$1" "$(lower "$BUILD_TYPE")"; }

# ---------------------------------------------------------------------------- signing
SIGN_FPR=""
resolve_signing_key() {
    have gpg || die "--official needs gpg to sign the packages (install gnupg)"
    local key=$SIGN_KEY fpr
    if [ -z "$key" ]; then
        key=$(gpg --batch --list-secret-keys --with-colons 2>/dev/null | awk -F: '/^sec/{print $5; exit}')
        [ -n "$key" ] || die "--official: no GPG secret key found; create one, or pass --sign-key=KEYID"
    fi
    fpr=$(gpg --batch --with-colons --fingerprint "$key" 2>/dev/null | awk -F: '/^fpr/{print $10; exit}')
    [ -n "$fpr" ] || die "--official: GPG key '$key' not found"
    gpg --batch --list-secret-keys --with-colons "$fpr" >/dev/null 2>&1 || die "--official: no secret key for $fpr"
    SIGN_FPR=$fpr
}

# ---------------------------------------------------------------------------- configure / build
CFG_BDIR=""
configure_target() {
    local name=$1
    parse_target "$name"
    local bdir
    bdir=$(target_bdir "$name")
    CFG_BDIR=$bdir
    local native=no
    [ "$name" = "$HOST_NAME" ] && native=yes

    if [ "$CLEAN_FIRST" = yes ] && [ -d "$bdir" ]; then rm -rf "$bdir"; fi

    local args=(-S "$SRC_DIR" -B "$bdir" -G "$(pick_generator)"
                "-DCMAKE_BUILD_TYPE=$BUILD_TYPE"
                "-DCMAKE_INSTALL_PREFIX=$PREFIX"
                "-DLZ_BUILD_CHANNEL=$CHANNEL"
                "-DLZ_SIGNING_KEY=$SIGN_FPR")
    local on=ON off=OFF
    mkdir -p "$BUILD_ROOT"

    # JIT: explicit choice, else wherever the engine has a backend.
    case "$ENABLE_JIT" in
        yes) jit_supported "$T_ARCH" "$T_OS" || warn "$name has no JIT backend: --enable-jit has no effect there"
             args+=("-DLZ_ENABLE_JIT=$on") ;;
        no)  args+=("-DLZ_ENABLE_JIT=$off") ;;
        auto) if jit_supported "$T_ARCH" "$T_OS"; then args+=("-DLZ_ENABLE_JIT=$on"); else args+=("-DLZ_ENABLE_JIT=$off"); fi ;;
    esac
    args+=("-DLZ_ENABLE_LTO=$(onoff "$ENABLE_LTO")" "-DLZ_ENABLE_SANITIZERS=$(onoff "$ENABLE_SANITIZERS")"
           "-DLZ_STATIC=$(onoff "$ENABLE_STATIC")" "-DLZ_WARNINGS_AS_ERRORS=$(onoff "$WERROR")")
    if [ "$ENABLE_STATIC" = yes ] && [ "$T_OS" = darwin ]; then
        warn "macOS has no static executables: --enable-static is ignored for $name"
        args+=("-DLZ_STATIC=OFF")
    fi
    # Tests run on this machine; a cross build cannot run them.
    if [ "$ENABLE_TESTS" = yes ] && [ "$native" = yes ]; then
        args+=("-DLZ_BUILD_TESTS=$on")
    else
        args+=("-DLZ_BUILD_TESTS=$off")
    fi

    if [ "$native" = yes ]; then
        args+=("-DCMAKE_C_COMPILER=${CC:-$HOST_CC}")
    else
        find_cross_cc "$bdir" || return 2
        say "    toolchain: $T_CC_HOW"
        args+=("-DCMAKE_C_COMPILER=$T_CC" "-DCMAKE_SYSTEM_PROCESSOR=$T_ARCH")
        case "$T_OS" in
            linux) args+=(-DCMAKE_SYSTEM_NAME=Linux) ;;
            darwin) args+=(-DCMAKE_SYSTEM_NAME=Darwin "-DCMAKE_OSX_ARCHITECTURES=$T_ARCH") ;;
            windows) args+=(-DCMAKE_SYSTEM_NAME=Windows) ;;
            freebsd) args+=(-DCMAKE_SYSTEM_NAME=FreeBSD) ;;
        esac
        if [ -n "$SYSROOT" ]; then args+=("-DCMAKE_SYSROOT=$SYSROOT"); fi
    fi
    if [ "$native" = yes ]; then
        if [ -n "${CFLAGS:-}" ]; then args+=("-DCMAKE_C_FLAGS=$CFLAGS"); fi
        if [ -n "${LDFLAGS:-}" ]; then args+=("-DCMAKE_EXE_LINKER_FLAGS=$LDFLAGS"); fi
    fi

    run cmake "${args[@]}" >"$bdir.configure.log" 2>&1 || {
        # the log lives beside the build directory so that it survives --clean-first
        sed 's/^/    | /' "$bdir.configure.log" >&2 || true
        die "configure failed for $name (log: $bdir.configure.log)"
    }
    return 0
}

build_target() {
    local name=$1 bdir
    bdir=$(target_bdir "$name")
    if [ "$VERBOSE" -eq 1 ]; then
        run cmake --build "$bdir" --parallel "$(cpu_count)"
    else
        run cmake --build "$bdir" --parallel "$(cpu_count)" >"$bdir.build.log" 2>&1 || {
            tail -n 40 "$bdir.build.log" | sed 's/^/    | /' >&2
            die "build failed for $name (log: $bdir.build.log)"
        }
    fi
}

read_buildinfo() { # dir key
    sed -n "s/^$2=//p" "$1/generated/BUILDINFO" | head -1
}

check_target() {
    local name=$1 bdir ctest_args
    bdir=$(target_bdir "$name")
    parse_target "$name"
    [ "$name" = "$HOST_NAME" ] || die "cannot run the tests of $name on $HOST_NAME"
    [ "$ENABLE_TESTS" = yes ] || die "--disable-tests and check do not go together"
    ctest_args=(--test-dir "$bdir" --output-on-failure -j "$(cpu_count)")
    # skink_cli insists that the JIT is active: without a backend it fails on purpose.
    if [ "$(read_buildinfo "$bdir" jit)" != on ]; then ctest_args+=(-E skink_cli); fi
    local log="$bdir.check.log" rc=0
    run ctest "${ctest_args[@]}" >"$log" 2>&1 || rc=$?
    if [ "$rc" -ne 0 ]; then
        cat "$log" >&2
        die "tests failed for $name (log: $log)"
    fi
    tail -n 3 "$log" | sed 's/^/    /' >&2
}

# ---------------------------------------------------------------------------- packaging
sha256_of() {
    if have sha256sum; then sha256sum "$@"; else shasum -a 256 "$@"; fi
}

make_tarball() { # stage_dir name out_file
    local stage=$1 name=$2 out=$3 epoch tar_flags=() compressor
    epoch=$(source_date_epoch)
    if tar --version 2>/dev/null | grep -q "GNU tar"; then
        tar_flags=(--sort=name "--mtime=@$epoch" --owner=0 --group=0 --numeric-owner)
    else
        warn "tar is not GNU tar: the package will not be byte-for-byte reproducible"
    fi
    if [ "$COMPRESS" = xz ]; then compressor=(xz -9e -T1 -c); else compressor=(gzip -9 -n -c); fi
    have "${compressor[0]}" || die "${compressor[0]} is needed for --compress=$COMPRESS"
    (cd "$stage" && tar "${tar_flags[@]+"${tar_flags[@]}"}" -cf - "$name") | "${compressor[@]}" >"$out"
}

package_target() {
    local name=$1 bdir stage version pkg out strip=()
    bdir=$(target_bdir "$name")
    version=$(read_buildinfo "$bdir" version)
    [ -n "$version" ] || die "no BUILDINFO in $bdir (did the build run?)"
    pkg="lizard-$version-$name"
    stage="$BUILD_ROOT/stage/$name"
    rm -rf "$stage"; mkdir -p "$stage" "$OUT_DIR"

    local strip_tool
    strip_tool=$(sed -n 's/^CMAKE_STRIP:[A-Z]*=//p' "$bdir/CMakeCache.txt" | head -1)
    if [ -n "$strip_tool" ] && [ "${strip_tool%-NOTFOUND}" = "$strip_tool" ] && [ "$BUILD_TYPE" != Debug ]; then
        strip=(--strip)
    fi
    run cmake --install "$bdir" --prefix "$stage/$pkg" "${strip[@]+"${strip[@]}"}" >/dev/null
    cp "$bdir/generated/BUILDINFO" "$stage/$pkg/BUILDINFO"

    # Smoke test of what is about to be shipped, when it can run here.
    if [ "$name" = "$HOST_NAME" ]; then
        local got
        got=$("$stage/$pkg/bin/lizard" --version | head -1)
        [ "$got" = "$version" ] || die "the packaged binary says '$got', expected '$version'"
    fi

    out="$OUT_DIR/$pkg.tar.$COMPRESS"
    make_tarball "$stage" "$pkg" "$out"
    say "    $out"
    PACKAGES+=("$out")
}

finish_packages() {
    [ "${#PACKAGES[@]}" -gt 0 ] || return 0
    local sums="$OUT_DIR/SHA256SUMS" f
    (cd "$OUT_DIR" && sha256_of $(ls lizard-*.tar.* 2>/dev/null | grep -v '\.asc$' | sort)) >"$sums"
    say "    $sums"
    if [ "$CHANNEL" = official ]; then
        step "Signing with $SIGN_FPR"
        for f in "${PACKAGES[@]}" "$sums"; do
            run gpg --batch --yes --armor --detach-sign --local-user "$SIGN_FPR" -o "$f.asc" "$f"
            say "    $f.asc"
        done
    fi
}

# ---------------------------------------------------------------------------- commands
cmd_detect() {
    local gen jobs libc="" t jit=no
    gen=$(pick_generator); jobs=$(cpu_count)
    jit_supported "$HOST_ARCH" "$HOST_OS" && jit=yes
    case "$HOST_ABI" in
        gnu*) libc=glibc; if have getconf; then libc="glibc $(getconf GNU_LIBC_VERSION 2>/dev/null | awk '{print $2}')"; fi ;;
        musl*) libc=musl ;;
    esac
    if [ "$PORCELAIN" = yes ]; then
        printf 'host=%s\narch=%s\nos=%s\nabi=%s\ncc=%s\ntriple=%s\ngenerator=%s\njobs=%s\njit=%s\n' \
            "$HOST_NAME" "$HOST_ARCH" "$HOST_OS" "$HOST_ABI" "$HOST_CC" "$HOST_TRIPLE" "$gen" "$jobs" "$jit"
        return
    fi
    printf 'Build machine\n'
    printf '  target name   %s   (arch %s, os %s%s)\n' "$HOST_NAME" "$HOST_ARCH" "$HOST_OS" "${HOST_ABI:+, abi $HOST_ABI}"
    [ -n "$libc" ] && printf '  C library     %s\n' "$libc"
    printf '  compiler      %s   (%s)\n' "$HOST_CC_VERSION" "$HOST_TRIPLE"
    if [ "$jit" = yes ]; then printf '  JIT           supported\n'; else printf '  JIT           not available (the GE interpreter runs everything)\n'; fi
    printf 'Tools\n'
    printf '  cmake         %s\n' "$(have cmake && cmake --version | head -1 || echo 'MISSING')"
    printf '  generator     %s, %s jobs\n' "$gen" "$jobs"
    printf '  git           %s\n' "$(have git && git --version || echo 'not installed (builds will be kind "source")')"
    printf '  tar           %s\n' "$(tar --version 2>/dev/null | head -1 || echo 'MISSING')"
    printf '  gpg           %s\n' "$(have gpg && gpg --version | head -1 || echo 'not installed (needed only for --official)')"
    if [ "$GIT_OK" = yes ]; then
        local state=clean
        tree_is_dirty && state="dirty (an --official build would be refused)"
        printf 'Source\n  commit        %s, tree %s\n' "$(git -C "$SRC_DIR" describe --tags --always 2>/dev/null || echo '?')" "$state"
    else
        printf 'Source\n  no git repository here%s\n' "$([ -f "$SRC_DIR/.buildinfo" ] && echo ' (a .buildinfo gives the commit)')"
    fi
    printf 'Targets for `package --targets=...` (from this machine)\n'
    local item
    for item in $DEFAULT_MATRIX; do
        parse_target "$item"
        if [ "$T_NAME" = "$HOST_NAME" ]; then
            printf '  %-26s native\n' "$T_NAME"
        else
            CROSS_CC_FROM_ENV=no
            if find_cross_cc "$BUILD_ROOT/.probe-$T_NAME"; then
                printf '  %-26s %s\n' "$T_NAME" "cross: $T_CC_HOW"
            else
                printf '  %-26s %s\n' "$T_NAME" "no toolchain (install $T_GNU-gcc, or zig)"
            fi
            rm -rf "$BUILD_ROOT/.probe-$T_NAME"
        fi
    done
}

prepare_channel() {
    if [ "$CHANNEL" = official ]; then
        resolve_signing_key
        if [ "$GIT_OK" = yes ] && tree_is_dirty; then
            die "--official needs a clean tree: commit or stash your changes first"
        fi
        [ "$GIT_OK" = yes ] || [ -f "$SRC_DIR/.buildinfo" ] || die "--official needs git or a .buildinfo to know the commit"
        if [ "$GIT_OK" = yes ] && ! git -C "$SRC_DIR" describe --tags --exact-match --match 'v[0-9]*' >/dev/null 2>&1; then
            warn "HEAD is not tagged vX.Y.Z: this official build will be a snapshot, not a release"
        fi
    else
        SIGN_FPR=""
    fi
}

run_targets() { # action: configure | build | check | package
    local action=$1 name built=0 skipped=0 failed=0 rc
    PACKAGES=()
    expand_targets
    for name in "${TARGET_LIST[@]}"; do
        step "$action $name ($BUILD_TYPE, $CHANNEL)"
        mkdir -p "$BUILD_ROOT"
        set +e
        ( set -e
          configure_target "$name" ) ; rc=$?
        set -e
        if [ "$rc" -eq 2 ]; then
            parse_target "$name"
            if [ "$STRICT" = yes ]; then die "no toolchain for $name (need $T_GNU-gcc, zig, or clang with --sysroot=)"; fi
            warn "skipping $name: no toolchain (need $T_GNU-gcc, zig, or clang with --sysroot=)"
            skipped=$((skipped + 1)); continue
        elif [ "$rc" -ne 0 ]; then
            failed=$((failed + 1)); [ "$KEEP_GOING" = yes ] || exit 1; continue
        fi
        [ "$action" = configure ] && { built=$((built + 1)); continue; }
        build_target "$name"
        case "$action" in
            check) check_target "$name" ;;
            package)
                if [ "$RUN_CHECK" = yes ]; then
                    if [ "$name" = "$HOST_NAME" ]; then check_target "$name"; else warn "--check skipped for $name: it cannot run here"; fi
                fi
                package_target "$name" ;;
            build) say "    $(target_bdir "$name")/bin/lizard" ;;
        esac
        built=$((built + 1))
    done
    if [ "$action" = package ]; then finish_packages; fi
    say ""
    say "$built built, $skipped skipped, $failed failed"
    [ "$built" -gt 0 ] || die "nothing was built"
    [ "$failed" -eq 0 ] || exit 1
}

cmd_sourcetar() {
    [ "$GIT_OK" = yes ] || die "sourcetar needs a git checkout"
    if tree_is_dirty; then die "sourcetar archives the committed tree: commit your changes first"; fi
    have cmake || die "cmake is needed to compute the build information"
    local probe="$BUILD_ROOT/.srcprobe" version out tmp
    rm -rf "$probe"; mkdir -p "$probe" "$OUT_DIR"
    run cmake -S "$SRC_DIR" -B "$probe" -DLZ_BUILD_TESTS=OFF >/dev/null
    version=$(read_buildinfo "$probe" version)
    tmp=$(mktemp -d)
    cp "$probe/generated/BUILDINFO" "$tmp/.buildinfo"
    out="$OUT_DIR/lizard-$version-src.tar.gz"
    run git -C "$SRC_DIR" archive --format=tar --prefix="lizard-$version/" --add-file="$tmp/.buildinfo" HEAD | gzip -9 -n >"$out"
    rm -rf "$tmp" "$probe"
    say "$out"
    (cd "$OUT_DIR" && sha256_of "$(basename "$out")" >"$(basename "$out").sha256")
}

cmd_verify() {
    local dir=${ARGS_REST[0]:-$OUT_DIR} status=0 f out fpr good=0
    [ -d "$dir" ] || die "no such directory: $dir"
    [ -f "$dir/SHA256SUMS" ] || die "$dir/SHA256SUMS not found"
    step "Checksums"
    if have sha256sum; then (cd "$dir" && sha256sum -c SHA256SUMS) || status=1
    else (cd "$dir" && shasum -a 256 -c SHA256SUMS) || status=1; fi
    step "Signatures"
    have gpg || die "gpg is needed to check signatures"
    local count=0
    for f in "$dir"/*.asc; do
        [ -e "$f" ] || break
        count=$((count + 1))
        out=$(gpg --batch --status-fd=1 --verify "$f" "${f%.asc}" 2>/dev/null || true)
        fpr=$(printf '%s\n' "$out" | awk '/^\[GNUPG:\] VALIDSIG/{print $3; exit}')
        if [ -z "$fpr" ]; then
            printf 'BAD or unknown key: %s\n' "$(basename "$f")"
            status=1
        elif [ -n "$EXPECT_KEY" ] && [ "$fpr" != "$(printf '%s' "$EXPECT_KEY" | tr -d ' ' | tr 'a-z' 'A-Z')" ]; then
            printf 'WRONG KEY: %s signed by %s\n' "$(basename "$f")" "$fpr"
            status=1
        else
            printf 'good: %s (%s)\n' "$(basename "$f")" "$fpr"
            good=$((good + 1))
        fi
    done
    if [ "$count" -eq 0 ]; then
        printf 'no .asc files: this is an unsigned (local) set\n'
        [ -z "$EXPECT_KEY" ] || status=1
    fi
    return $status
}

cmd_clean() {
    case "$BUILD_ROOT" in ''|/|"$HOME") die "refusing to remove '$BUILD_ROOT'" ;; esac
    rm -rf "$BUILD_ROOT"
    say "removed $BUILD_ROOT"
}

# ---------------------------------------------------------------------------- main
main() {
    parse_args "$@"
    case "$COMMAND" in
        help) usage; exit 0 ;;
        clean) cmd_clean; exit 0 ;;
        verify) cmd_verify; exit $? ;;
    esac
    have cmake || die "cmake (3.16 or newer) is required"
    detect_host
    detect_git
    case "$COMMAND" in
        detect) cmd_detect ;;
        configure|build|check)
            prepare_channel
            run_targets "$COMMAND" ;;
        package)
            prepare_channel
            run_targets package ;;
        sourcetar) cmd_sourcetar ;;
        *) die "unknown command: $COMMAND (try --help)" ;;
    esac
}

main "$@"
