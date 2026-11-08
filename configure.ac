dnl Lizard: GNU Autotools build. Non-recursive automake, no libtool.
dnl From git:    ./autogen.sh && ./configure && make
dnl From a dist: ./configure && make
AC_PREREQ([2.69])
AC_INIT([lizard], [0.1.0], [https://github.com/OhMyDitzzy/lizard-lang/issues],
        [lizard], [https://github.com/OhMyDitzzy/lizard-lang])
AC_CONFIG_SRCDIR([src/cli/main.c])
AC_CANONICAL_HOST
AM_INIT_AUTOMAKE([foreign subdir-objects silent-rules parallel-tests -Wall])
AM_SILENT_RULES

dnl ---- options ------------------------------------------------------------------------------
AC_ARG_WITH([build-type],
    [AS_HELP_STRING([--with-build-type=TYPE],
        [release (default), debug, relwithdebinfo or minsizerel; ignored when CFLAGS is given])],
    [lz_build_type=$withval], [lz_build_type=release])
case $lz_build_type in
    release|debug|relwithdebinfo|minsizerel) ;;
    *) AC_MSG_ERROR([bad --with-build-type=$lz_build_type (release, debug, relwithdebinfo, minsizerel)]) ;;
esac

AC_ARG_ENABLE([jit],
    [AS_HELP_STRING([--enable-jit@<:@=auto|yes|no@:>@],
        [the JIT of the Gecko Engine (default: auto = x86_64 and aarch64 on Linux and macOS)])],
    [], [enable_jit=auto])
AC_ARG_ENABLE([lto], [AS_HELP_STRING([--enable-lto], [link-time optimization (-flto)])], [], [enable_lto=no])
AC_ARG_ENABLE([sanitizers],
    [AS_HELP_STRING([--enable-sanitizers], [AddressSanitizer and UBSan])], [], [enable_sanitizers=no])
AC_ARG_ENABLE([poison],
    [AS_HELP_STRING([--enable-poison], [poison and quarantine freed objects (-DLZ_POISON, see docs/MEMORY.md)])],
    [], [enable_poison=no])
AC_ARG_ENABLE([static-binary],
    [AS_HELP_STRING([--enable-static-binary], [link the lizard executable statically (-static)])],
    [], [enable_static_binary=no])
AC_ARG_ENABLE([werror], [AS_HELP_STRING([--enable-werror], [treat compiler warnings as errors])], [], [enable_werror=no])
AC_ARG_ENABLE([tests],
    [AS_HELP_STRING([--disable-tests], [do not build the tests ("make check" only says so)])], [], [enable_tests=yes])
AC_ARG_ENABLE([official],
    [AS_HELP_STRING([--enable-official],
        [official (signed) channel: clean tree and known commit required; needs --with-signing-key])],
    [], [enable_official=no])
AC_ARG_WITH([signing-key],
    [AS_HELP_STRING([--with-signing-key=FPR], [fingerprint of the key that signs an official build's package])],
    [lz_signing_key=$withval], [lz_signing_key=])

for lz_opt in lto sanitizers poison static_binary werror tests official; do
    eval lz_val=\$enable_$lz_opt
    case $lz_val in
        yes|no) ;;
        *) AC_MSG_ERROR([bad value for --enable-$lz_opt: $lz_val (use yes or no)]) ;;
    esac
done
case $enable_jit in auto|yes|no) ;; *) AC_MSG_ERROR([bad value for --enable-jit: $enable_jit]) ;; esac
if test "$enable_sanitizers" = yes && test "$enable_static_binary" = yes; then
    AC_MSG_ERROR([--enable-sanitizers and --enable-static-binary cannot be combined])
fi
if test "$lz_signing_key" = yes || test "$lz_signing_key" = no; then
    AC_MSG_ERROR([--with-signing-key needs a key fingerprint])
fi

dnl ---- compiler ------------------------------------------------------------------------------
dnl A CFLAGS from the user is respected as it is; otherwise --with-build-type picks the flags.
lz_user_cflags=${CFLAGS+set}
: ${CFLAGS=}
AC_PROG_CC
AM_PROG_AR
AC_PROG_RANLIB
AC_PROG_INSTALL
AC_PROG_MKDIR_P

lz_config=custom
if test "$lz_user_cflags" != set; then
    case $lz_build_type in
        release)        CFLAGS="-O2 -DNDEBUG";    lz_config=Release ;;
        debug)          CFLAGS="-g -O0";          lz_config=Debug ;;
        relwithdebinfo) CFLAGS="-O2 -g -DNDEBUG"; lz_config=RelWithDebInfo ;;
        minsizerel)     CFLAGS="-Os -DNDEBUG";    lz_config=MinSizeRel ;;
    esac
fi

dnl C11, without GNU extensions
AC_MSG_CHECKING([whether $CC accepts -std=c11])
lz_save_CFLAGS=$CFLAGS
CFLAGS="$CFLAGS -std=c11"
AC_COMPILE_IFELSE([AC_LANG_SOURCE([[
#if !defined(__STDC_VERSION__) || __STDC_VERSION__ < 201112L
#error "C11 needed"
#endif
int main(void) { return 0; }
]])], [AC_MSG_RESULT([yes])],
      [AC_MSG_RESULT([no]); AC_MSG_ERROR([a C11 compiler is required])])
CFLAGS=$lz_save_CFLAGS
LZ_STD_CFLAGS="-std=c11"

dnl warnings: only those the compiler knows, tried one by one (-Werror makes clang reject unknown ones)
LZ_WARN_CFLAGS=
for lz_flag in -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wstrict-prototypes -Wmissing-prototypes -Wformat=2; do
    AS_VAR_PUSHDEF([lz_cv], [lz_cv_cflag_$lz_flag])
    AC_CACHE_CHECK([whether $CC accepts $lz_flag], [lz_cv], [
        lz_save_CFLAGS=$CFLAGS
        CFLAGS="$CFLAGS -std=c11 -Werror $lz_flag"
        AC_COMPILE_IFELSE([AC_LANG_SOURCE([[int main(void) { return 0; }]])],
            [AS_VAR_SET([lz_cv], [yes])], [AS_VAR_SET([lz_cv], [no])])
        CFLAGS=$lz_save_CFLAGS])
    AS_VAR_IF([lz_cv], [yes], [LZ_WARN_CFLAGS="$LZ_WARN_CFLAGS $lz_flag"])
    AS_VAR_POPDEF([lz_cv])
done
if test "$enable_werror" = yes; then
    LZ_WARN_CFLAGS="$LZ_WARN_CFLAGS -Werror"
fi

dnl -lm
AC_SEARCH_LIBS([fmod], [m], [], [AC_MSG_ERROR([the math library (-lm) is needed])])

dnl ---- feature switches ------------------------------------------------------------------------
dnl JIT: only where the engine has a backend (src/engine/ge_jit.h)
lz_jit_platform=no
case $host_cpu:$host_os in
    x86_64:linux*|x86_64:darwin*|aarch64:linux*|aarch64:darwin*|arm64:linux*|arm64:darwin*)
        lz_jit_platform=yes ;;
esac
case $enable_jit in
    auto)
        lz_jit=$lz_jit_platform
        if test "$lz_jit" = no; then
            AC_MSG_NOTICE([no JIT backend for $host_cpu-$host_os: the GE interpreter runs everything (-DLZ_NO_JIT)])
        fi ;;
    yes)
        lz_jit=$lz_jit_platform
        if test "$lz_jit" = no; then
            AC_MSG_WARN([--enable-jit has no effect: there is no JIT backend for $host_cpu-$host_os (building with -DLZ_NO_JIT)])
        fi ;;
    no) lz_jit=no ;;
esac
LZ_JIT_CPPFLAGS=
if test "$lz_jit" = no; then LZ_JIT_CPPFLAGS="-DLZ_NO_JIT"; fi

LZ_POISON_CPPFLAGS=
if test "$enable_poison" = yes; then LZ_POISON_CPPFLAGS="-DLZ_POISON"; fi

LZ_SAN_CFLAGS=; LZ_SAN_LDFLAGS=
if test "$enable_sanitizers" = yes; then
    AC_MSG_CHECKING([whether $CC supports -fsanitize=address,undefined])
    lz_save_CFLAGS=$CFLAGS; lz_save_LDFLAGS=$LDFLAGS
    CFLAGS="$CFLAGS -fsanitize=address,undefined -fno-omit-frame-pointer"
    LDFLAGS="$LDFLAGS -fsanitize=address,undefined"
    AC_LINK_IFELSE([AC_LANG_SOURCE([[int main(void) { return 0; }]])],
        [AC_MSG_RESULT([yes])],
        [AC_MSG_RESULT([no]); AC_MSG_ERROR([--enable-sanitizers: $CC cannot build with ASan and UBSan here])])
    CFLAGS=$lz_save_CFLAGS; LDFLAGS=$lz_save_LDFLAGS
    LZ_SAN_CFLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer"
    LZ_SAN_LDFLAGS="-fsanitize=address,undefined"
fi

LZ_LTO_CFLAGS=; LZ_LTO_LDFLAGS=; lz_lto=no
if test "$enable_lto" = yes; then
    AC_MSG_CHECKING([whether $CC supports -flto])
    lz_save_CFLAGS=$CFLAGS; lz_save_LDFLAGS=$LDFLAGS
    CFLAGS="$CFLAGS -flto"; LDFLAGS="$LDFLAGS -flto"
    AC_LINK_IFELSE([AC_LANG_SOURCE([[int main(void) { return 0; }]])],
        [AC_MSG_RESULT([yes]); lz_lto=yes; LZ_LTO_CFLAGS="-flto"; LZ_LTO_LDFLAGS="-flto"],
        [AC_MSG_RESULT([no]); AC_MSG_WARN([LTO requested but not supported by $CC: building without it])])
    CFLAGS=$lz_save_CFLAGS; LDFLAGS=$lz_save_LDFLAGS
fi

LZ_STATIC_LDFLAGS=
if test "$enable_static_binary" = yes; then
    case $host_os in
        darwin*) AC_MSG_WARN([macOS has no static executables: --enable-static-binary is ignored])
                 enable_static_binary=no ;;
        *) LZ_STATIC_LDFLAGS="-static" ;;
    esac
fi

dnl ---- tests ------------------------------------------------------------------------------------
lz_run_tests=$enable_tests
if test "$cross_compiling" = yes && test "$lz_run_tests" = yes; then
    AC_MSG_NOTICE([cross compiling: "make check" will be skipped])
    lz_run_tests=no
fi
AM_CONDITIONAL([LZ_RUN_TESTS], [test "$lz_run_tests" = yes])
AM_CONDITIONAL([LZ_STATIC_BINARY], [test "$enable_static_binary" = yes])

dnl ---- build information (commit, kind, channel) -----------------------------------------------
lz_channel=local
if test "$enable_official" = yes; then
    lz_channel=official
    if test -z "$lz_signing_key"; then
        AC_MSG_ERROR([--enable-official needs --with-signing-key=FPR])
    fi
fi
lz_abs_srcdir=`cd "$srcdir" && pwd`
lz_abs_builddir=`pwd`
LZ_BUILDINFO_ARGS="--srcdir='$lz_abs_srcdir' --builddir='$lz_abs_builddir' --version='$PACKAGE_VERSION' --channel='$lz_channel' --signing-key='$lz_signing_key' --config='$lz_config' --cc='$CC' --jit=$lz_jit --lto=$lz_lto --sanitizers=$enable_sanitizers --static=$enable_static_binary"
# generated now, so that the header exists after configure and a bad official request fails early
AC_MSG_CHECKING([the build information])
if lz_bi=`eval "$SHELL '$lz_abs_srcdir/tools/buildinfo.sh' $LZ_BUILDINFO_ARGS" 2>&1`; then
    AC_MSG_RESULT([ok])
else
    AC_MSG_RESULT([failed])
    AC_MSG_ERROR([$lz_bi])
fi
test -z "$lz_bi" || AC_MSG_WARN([$lz_bi])
lz_version=`sed -n 's/^version=//p' generated/BUILDINFO`
lz_kind=`sed -n 's/^kind=//p' generated/BUILDINFO`

AC_SUBST([LZ_STD_CFLAGS])
AC_SUBST([LZ_WARN_CFLAGS])
AC_SUBST([LZ_SAN_CFLAGS])
AC_SUBST([LZ_SAN_LDFLAGS])
AC_SUBST([LZ_LTO_CFLAGS])
AC_SUBST([LZ_LTO_LDFLAGS])
AC_SUBST([LZ_STATIC_LDFLAGS])
AC_SUBST([LZ_JIT_CPPFLAGS])
AC_SUBST([LZ_POISON_CPPFLAGS])
AC_SUBST([LZ_BUILDINFO_ARGS])

AC_CONFIG_FILES([Makefile])
AC_OUTPUT

AC_MSG_NOTICE([
Lizard $lz_version ($lz_kind, $lz_config, channel $lz_channel)
  host:       $host
  compiler:   $CC
  tests:      $enable_tests (make check runs them: $lz_run_tests)
  sanitizers: $enable_sanitizers
  poison:     $enable_poison
  werror:     $enable_werror
  lto:        $lz_lto
  jit:        $lz_jit
  static:     $enable_static_binary
])
