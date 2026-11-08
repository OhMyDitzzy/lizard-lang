#!/bin/sh
# Writes generated/lizard/version.h and generated/BUILDINFO (the port of what used to be
# cmake/GenerateBuildInfo.cmake). Run by `configure`, on every `make`, and by
# tools/build-plain.sh. Files are only rewritten when their content changes, so nothing is
# recompiled unless the build information really changed.
#
#   tools/buildinfo.sh --srcdir=DIR --builddir=DIR --version=0.1.0 [--channel=local|official]
#       [--signing-key=FPR] [--config=Release] [--cc=CC] [--jit=on|off] [--lto=on|off]
#       [--sanitizers=on|off] [--static=on|off] [--outdir=DIR] [--template=FILE]
#   tools/buildinfo.sh --srcdir=DIR --version=0.1.0 --dist-info=FILE     (writes a .buildinfo)
#
# Build kind:
#   release   clean tree, HEAD is exactly the tag vX.Y.Z
#   snapshot  clean tree, HEAD is past the last tag (or there is no tag)
#   dirty     uncommitted changes (the version carries a hash of them, so two dirty builds of
#             different edits do not look alike)
#   source    no git: a source tarball. `make dist` puts a .buildinfo next to the sources, which
#             this script then reads; without it the commit is "unknown".

set -u

srcdir=; builddir=; version=; channel=local; signing_key=; config=; cc=cc
jit=off; lto=off; sanitizers=off; static=off; outdir=; template=; dist_info=

die() { printf 'buildinfo: error: %s\n' "$*" >&2; exit 1; }
warn() { printf 'buildinfo: warning: %s\n' "$*" >&2; }
have() { command -v "$1" >/dev/null 2>&1; }

for arg in "$@"; do
    case $arg in
        --srcdir=*) srcdir=${arg#*=} ;;
        --builddir=*) builddir=${arg#*=} ;;
        --version=*) version=${arg#*=} ;;
        --channel=*) channel=${arg#*=} ;;
        --signing-key=*) signing_key=${arg#*=} ;;
        --config=*) config=${arg#*=} ;;
        --cc=*) cc=${arg#*=} ;;
        --jit=*) jit=${arg#*=} ;;
        --lto=*) lto=${arg#*=} ;;
        --sanitizers=*) sanitizers=${arg#*=} ;;
        --static=*) static=${arg#*=} ;;
        --outdir=*) outdir=${arg#*=} ;;
        --template=*) template=${arg#*=} ;;
        --dist-info=*) dist_info=${arg#*=} ;;
        *) die "unknown argument: $arg" ;;
    esac
done
[ -n "$srcdir" ] || die "--srcdir is required"
[ -n "$version" ] || die "--version is required"
[ -n "$channel" ] || channel=local
[ -n "$builddir" ] || builddir=$srcdir
[ -n "$outdir" ] || outdir=$builddir/generated
[ -n "$template" ] || template=$srcdir/include/lizard/version.h.in
srcdir=$(cd "$srcdir" && pwd) || die "no such source directory"
mkdir -p "$builddir" && builddir=$(cd "$builddir" && pwd)

core=$version
major=${core%%.*}; rest=${core#*.}; minor=${rest%%.*}; patch=${rest#*.}; patch=${patch%%[!0-9]*}

# ---- helpers ------------------------------------------------------------------------------
sha256_stdin() { # prints the hex digest of stdin
    if have sha256sum; then sha256sum | awk '{print $1}'
    elif have shasum; then shasum -a 256 | awk '{print $1}'
    elif have openssl; then openssl dgst -sha256 | awk '{print $NF}'
    else cksum | awk '{printf "%08x%s", $1, "00000000"}'   # last resort: 32 bit CRC, padded
    fi
}

epoch_to_iso() { # seconds since the epoch -> 2026-01-31T12:00:00Z
    t=$1
    out=$(date -u -d "@$t" '+%Y-%m-%dT%H:%M:%SZ' 2>/dev/null) && { printf '%s\n' "$out"; return; }
    out=$(date -u -r "$t" '+%Y-%m-%dT%H:%M:%SZ' 2>/dev/null) && { printf '%s\n' "$out"; return; }
    awk -v t="$t" 'BEGIN {   # civil-from-days, no date(1) needed
        d = int(t / 86400); s = t - d * 86400
        z = d + 719468; era = int(z / 146097); doe = z - era * 146097
        yoe = int((doe - int(doe / 1460) + int(doe / 36524) - int(doe / 146096)) / 365)
        y = yoe + era * 400; doy = doe - (365 * yoe + int(yoe / 4) - int(yoe / 100))
        mp = int((5 * doy + 2) / 153); dd = doy - int((153 * mp + 2) / 5) + 1
        m = mp < 10 ? mp + 3 : mp - 9; if (m <= 2) y++
        printf "%04d-%02d-%02dT%02d:%02d:%02dZ\n", y, m, dd, int(s / 3600), int(s % 3600 / 60), s % 60 }'
}

git_in_src() { git -C "$srcdir" "$@" 2>/dev/null; }

c_string() { printf '%s' "$1" | tr '\n' ' ' | sed -e 's/\\/\\\\/g' -e 's/"/\\"/g'; }

# ---- the compiler and the target -------------------------------------------------------------
compiler_id() {
    macros=$($cc -dM -E -x c /dev/null 2>/dev/null) || macros=$(echo | $cc -dM -E - 2>/dev/null) || macros=
    mac() { printf '%s\n' "$macros" | sed -n "s/^#define $1 //p" | head -1; }
    if [ -n "$(mac __clang__)" ]; then
        id=Clang; [ -n "$(mac __apple_build_version__)" ] && id=AppleClang
        printf '%s %s.%s.%s' "$id" "$(mac __clang_major__)" "$(mac __clang_minor__)" "$(mac __clang_patchlevel__)"
    elif [ -n "$(mac __GNUC__)" ]; then
        printf 'GNU %s.%s.%s' "$(mac __GNUC__)" "$(mac __GNUC_MINOR__)" "$(mac __GNUC_PATCHLEVEL__)"
    elif [ -n "$(mac __TINYC__)" ]; then
        printf 'TinyCC'
    else
        printf '%s' "${cc%% *}"
    fi
}
triple=$($cc -dumpmachine 2>/dev/null) || triple=
[ -n "$triple" ] || triple=$(printf '%s-%s' "$(uname -m)" "$(uname -s)" | tr 'A-Z' 'a-z')
compiler=$(compiler_id)

# ---- the commit and the state of the tree -------------------------------------------------------
kind=source; commit=unknown; commit_short=unknown; commit_date=unknown
dirty=0; dirty_hash=; commits_since=0; tag=; orig_kind=source

# Build directories are not part of the source: a build tree inside srcdir that is not ignored
# must not turn the tree dirty. Prefixes relative to srcdir, with a trailing slash.
build_prefixes=
case "$builddir/" in
    "$srcdir"/?*) build_prefixes="${builddir#"$srcdir"/}/" ;;
esac

in_build_prefix() { # file
    for p in $build_prefixes; do
        case "$1" in "$p"*) return 0 ;; esac
    done
    return 1
}

git_ok=no
if have git; then
    prefix=$(git -C "$srcdir" rev-parse --show-prefix 2>/dev/null) && prefix_rc=0 || prefix_rc=1
    inside=$(git_in_src rev-parse --is-inside-work-tree) || inside=
    # Only a repository rooted at srcdir counts: sources unpacked inside some other checkout must
    # not report that checkout's commit.
    if [ "$prefix_rc" -eq 0 ] && [ "$inside" = true ] && [ -z "$prefix" ]; then git_ok=yes; fi
fi

if [ "$git_ok" = yes ]; then
    if commit=$(git_in_src rev-parse HEAD) && [ -n "$commit" ]; then
        commit_short=$(git_in_src rev-parse --short=7 HEAD)
        commit_date=$(git_in_src show -s --format=%cI HEAD)
        if tag=$(git_in_src describe --tags --abbrev=0 --match 'v[0-9]*') && [ -n "$tag" ]; then
            commits_since=$(git_in_src rev-list --count "$tag..HEAD")
        else
            tag=
            commits_since=$(git_in_src rev-list --count HEAD)
        fi
        changes=
        status=$(git_in_src status --porcelain --untracked-files=normal)
        nl='
'
        oldifs=$IFS; IFS=$nl
        for line in $status; do
            case $line in
                '?? '*/)
                    d=${line#'?? '}
                    if [ -e "$srcdir/${d}config.status" ] || [ -e "$srcdir/${d}.lizard-plain-build" ] \
                       || [ -e "$srcdir/${d}CMakeCache.txt" ]; then
                        build_prefixes="$build_prefixes $d"
                        continue
                    fi ;;
            esac
            changes="$changes$line$nl"
        done
        IFS=$oldifs
        if [ -n "$changes" ]; then
            dirty=1
            material=$(git_in_src diff HEAD)
            untracked=$(git_in_src ls-files --others --exclude-standard)
            IFS=$nl
            for f in $untracked; do
                IFS=$oldifs
                if ! in_build_prefix "$f" && [ -f "$srcdir/$f" ]; then
                    h=$(sha256_stdin <"$srcdir/$f")
                    material="$material$nl$f $h"
                fi
                IFS=$nl
            done
            IFS=$oldifs
            dirty_hash=$(printf '%s' "$material" | sha256_stdin | cut -c1-8)
            kind=dirty
        elif [ -n "$tag" ] && [ "$commits_since" -eq 0 ]; then
            kind=release
        else
            kind=snapshot
        fi
        orig_kind=$kind
    else
        commit=unknown   # a repository with no commit yet
    fi
elif [ -f "$srcdir/.buildinfo" ]; then
    # A source tarball made by `make dist`: the state of the tree it was cut from. The build is
    # of kind "source" (no git here), but it knows the commit.
    info() { sed -n "s/^$1=//p" "$srcdir/.buildinfo" | head -1; }
    for key in kind commit commit_short commit_date dirty_hash commits_since; do
        v=$(info $key)
        [ -n "$v" ] || continue
        case $key in
            kind) orig_kind=$v ;;
            commit) commit=$v ;;
            commit_short) commit_short=$v ;;
            commit_date) commit_date=$v ;;
            dirty_hash) dirty_hash=$v ;;
            commits_since) commits_since=$v ;;
        esac
    done
    [ "$orig_kind" = dirty ] || dirty_hash=
fi

# SOURCE_DATE_EPOCH wins over the commit date, for reproducible packages.
if [ -n "${SOURCE_DATE_EPOCH:-}" ]; then
    commit_date=$(epoch_to_iso "$SOURCE_DATE_EPOCH")
fi

# `make dist`: remember the commit, so that a build without git still knows it.
if [ -n "$dist_info" ]; then
    if [ "$git_ok" = no ] && [ -f "$srcdir/.buildinfo" ]; then
        cp "$srcdir/.buildinfo" "$dist_info"   # a tarball being re-packed (make distcheck)
    else
        {
            printf 'kind=%s\ncommit=%s\ncommit_short=%s\ncommit_date=%s\n' \
                "$orig_kind" "$commit" "$commit_short" "$commit_date"
            printf 'dirty_hash=%s\ncommits_since=%s\n' "$dirty_hash" "$commits_since"
        } >"$dist_info"
    fi
    exit 0
fi

# ---- the version string ------------------------------------------------------------------------
case $kind in
    release)
        full=$core
        [ "$tag" = "v$core" ] || warn "HEAD is tagged $tag but the project version is $core" ;;
    snapshot) full="$core-dev.$commits_since+g$commit_short" ;;
    dirty)
        if [ "$commits_since" -gt 0 ]; then
            full="$core-dev.$commits_since+g$commit_short.dirty.$dirty_hash"
        else
            full="$core+g$commit_short.dirty.$dirty_hash"
        fi ;;
    *)
        if [ "$commit" = unknown ]; then full="$core+source"; else full="$core+g$commit_short"; fi
        [ -z "$dirty_hash" ] || full="$full.dirty.$dirty_hash" ;;
esac

# ---- channel and signature ---------------------------------------------------------------------
# "official" is the claim that this build is meant to be signed by the maintainers: it needs a
# clean tree (a dirty one is, by definition, not what anybody signed) and a signing key. The
# signature itself is a detached file next to the package (tools/release.sh package --official);
# the binary only says who is expected to have signed it.
case $channel in
    official)
        if [ "$kind" = dirty ] || { [ "$kind" = source ] && [ "$commit" = unknown ]; }; then
            die "An official build needs a clean tree with a known commit (this one is '$kind'). Commit your changes, or configure without --enable-official."
        fi
        [ -n "$signing_key" ] || die "An official build needs a signing key: --with-signing-key=FPR (tools/release.sh --official finds it for you)."
        signed=1; signer=$signing_key ;;
    local) signed=0; signer= ;;
    *) die "the channel must be 'local' or 'official', not '$channel'" ;;
esac

# ---- the rest ------------------------------------------------------------------------------------
onoff() { case $1 in on|ON|yes|1) echo on ;; *) echo off ;; esac; }
flag() { [ "$(onoff "$1")" = on ] && echo 1 || echo 0; }
if [ "$signed" -eq 1 ]; then
    note=" (signed, key $signer; verify the .asc of the package)"
else
    note=" (unsigned)"
fi
[ -n "$config" ] || config=custom

mkdir -p "$outdir/lizard"
[ -f "$template" ] || die "no template: $template"

esc() { printf '%s' "$1" | sed -e 's/[\\&|]/\\&/g'; }
tmp_h=$outdir/lizard/version.h.tmp.$$
tmp_i=$outdir/BUILDINFO.tmp.$$
sed \
    -e "s|@PROJECT_VERSION_MAJOR@|$(esc "$major")|g" \
    -e "s|@PROJECT_VERSION_MINOR@|$(esc "$minor")|g" \
    -e "s|@PROJECT_VERSION_PATCH@|$(esc "$patch")|g" \
    -e "s|@PROJECT_VERSION@|$(esc "$core")|g" \
    -e "s|@LZ_VERSION_FULL@|$(esc "$(c_string "$full")")|g" \
    -e "s|@LZ_BUILD_KIND@|$(esc "$(c_string "$kind")")|g" \
    -e "s|@LZ_BUILD_DIRTY_HASH@|$(esc "$(c_string "$dirty_hash")")|g" \
    -e "s|@LZ_BUILD_DIRTY@|$dirty|g" \
    -e "s|@LZ_BUILD_COMMIT_SHORT@|$(esc "$(c_string "$commit_short")")|g" \
    -e "s|@LZ_BUILD_COMMIT@|$(esc "$(c_string "$commit")")|g" \
    -e "s|@LZ_BUILD_DATE@|$(esc "$(c_string "$commit_date")")|g" \
    -e "s|@LZ_BUILD_CHANNEL_NOTE@|$(esc "$(c_string "$note")")|g" \
    -e "s|@LZ_BUILD_CHANNEL@|$(esc "$(c_string "$channel")")|g" \
    -e "s|@LZ_BUILD_SIGNED@|$signed|g" \
    -e "s|@LZ_BUILD_SIGNER@|$(esc "$(c_string "$signer")")|g" \
    -e "s|@LZ_BUILD_CONFIG@|$(esc "$(c_string "$config")")|g" \
    -e "s|@LZ_BUILD_TARGET@|$(esc "$(c_string "$triple")")|g" \
    -e "s|@LZ_BUILD_COMPILER@|$(esc "$(c_string "$compiler")")|g" \
    -e "s|@LZ_BUILD_JIT_TEXT@|$(onoff "$jit")|g" \
    -e "s|@LZ_BUILD_LTO_TEXT@|$(onoff "$lto")|g" \
    -e "s|@LZ_BUILD_SANITIZERS_TEXT@|$(onoff "$sanitizers")|g" \
    -e "s|@LZ_BUILD_STATIC_TEXT@|$(onoff "$static")|g" \
    -e "s|@LZ_BUILD_JIT@|$(flag "$jit")|g" \
    -e "s|@LZ_BUILD_LTO@|$(flag "$lto")|g" \
    -e "s|@LZ_BUILD_SANITIZERS@|$(flag "$sanitizers")|g" \
    -e "s|@LZ_BUILD_STATIC@|$(flag "$static")|g" \
    "$template" >"$tmp_h" || { rm -f "$tmp_h"; die "cannot write $tmp_h"; }

# The same facts as plain text: installed as share/doc/lizard/BUILDINFO and read by tools.
cat >"$tmp_i" <<EOF
version=$full
core_version=$core
kind=$kind
commit=$commit
commit_short=$commit_short
commit_date=$commit_date
commits_since=$commits_since
dirty=$dirty
dirty_hash=$dirty_hash
channel=$channel
signed=$signed
signer=$signer
config=$config
target=$triple
compiler=$compiler
jit=$(onoff "$jit")
lto=$(onoff "$lto")
sanitizers=$(onoff "$sanitizers")
static=$(onoff "$static")
EOF

install_if_changed() { # tmp dest
    if [ -f "$2" ] && cmp -s "$1" "$2"; then rm -f "$1"; else mv -f "$1" "$2"; fi
}
install_if_changed "$tmp_h" "$outdir/lizard/version.h"
install_if_changed "$tmp_i" "$outdir/BUILDINFO"
