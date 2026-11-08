# Lizard: non-recursive automake, no libtool (internal static libraries only).
# The file lists live in mk/sources.am and the test list in tests/tests.list (-> mk/tests.am), so
# that tools/build-plain.sh (no autotools, no make) reads the very same ones.

include $(top_srcdir)/mk/sources.am

bin_PROGRAMS = lizard
noinst_LIBRARIES = liblizard_core.a liblizard_frontend.a liblizard_engine.a \
	liblizard_builtin.a liblizard_utils.a

AM_CPPFLAGS = -I$(top_srcdir)/include -I$(top_srcdir)/builtin/include -I$(top_srcdir)/builtin \
	-Igenerated @LZ_POISON_CPPFLAGS@
# warnings and the language level go in AM_CFLAGS, so the user's CFLAGS stay untouched
AM_CFLAGS = @LZ_STD_CFLAGS@ @LZ_WARN_CFLAGS@ @LZ_SAN_CFLAGS@ @LZ_LTO_CFLAGS@
AM_LDFLAGS = @LZ_SAN_LDFLAGS@ @LZ_LTO_LDFLAGS@

liblizard_core_a_SOURCES = $(lizard_core_SOURCES)
liblizard_frontend_a_SOURCES = $(lizard_frontend_SOURCES)
liblizard_engine_a_SOURCES = $(lizard_engine_SOURCES)
liblizard_engine_a_CPPFLAGS = $(AM_CPPFLAGS) -I$(top_srcdir)/src/engine @LZ_JIT_CPPFLAGS@
liblizard_builtin_a_SOURCES = $(lizard_builtin_SOURCES)
liblizard_utils_a_SOURCES = $(lizard_utils_SOURCES)

# link order: engine, frontend, builtin, core, utils, -lm (LIBS)
lizard_LDADD = liblizard_engine.a liblizard_frontend.a liblizard_builtin.a \
	liblizard_core.a liblizard_utils.a
if LZ_STATIC_BINARY
lizard_LDFLAGS = $(AM_LDFLAGS) @LZ_STATIC_LDFLAGS@
endif

# ---- build information: regenerated on every make, rewritten only when it changed ----------------
BUILT_SOURCES = generated/lizard/version.h generated/BUILDINFO
lz_buildinfo = $(SHELL) $(top_srcdir)/tools/buildinfo.sh @LZ_BUILDINFO_ARGS@
generated/lizard/version.h: FORCE
	$(AM_V_GEN)$(lz_buildinfo)
generated/BUILDINFO: generated/lizard/version.h
	@test -f $@ || $(lz_buildinfo)
FORCE:
.PHONY: FORCE
DISTCLEANFILES = generated/lizard/version.h generated/BUILDINFO
distclean-local:
	-rmdir generated/lizard generated 2>/dev/null || true

# ---- tests -----------------------------------------------------------------------------------------
LZ_LDADD_PLAIN = liblizard_frontend.a liblizard_builtin.a liblizard_core.a
LZ_LDADD_ENGINE = liblizard_engine.a liblizard_frontend.a liblizard_builtin.a liblizard_core.a
lz_make_stub = printf '%s\n' '\#!/bin/sh' '\# stub: see tests/run-test.sh' >$@ && chmod +x $@
include $(top_srcdir)/mk/tests.am
if LZ_RUN_TESTS
LOG_COMPILER = $(SHELL) $(top_srcdir)/tests/run-test.sh
AM_TESTS_ENVIRONMENT = LZ_SRCDIR='$(abs_top_srcdir)'; export LZ_SRCDIR;
else
check-local:
	@echo "tests are disabled (--disable-tests) or this is a cross build: make check skipped"
endif
CLEANFILES = $(check_SCRIPTS)

# ---- bench ----------------------------------------------------------------------------------------
bench: all
	$(top_srcdir)/bench/run.sh ./lizard 3
.PHONY: bench

# ---- install -------------------------------------------------------------------------------------
# README.md and docs/**/*.md under share/doc/lizard, BUILDINFO beside them, examples in share/lizard
nobase_dist_doc_DATA = $(lizard_docs)
nodist_doc_DATA = generated/BUILDINFO
examplesdir = $(datadir)/lizard/examples
dist_examples_DATA = $(lizard_examples)

# ---- dist ----------------------------------------------------------------------------------------
EXTRA_DIST = autogen.sh LICENSE bench builtin docs examples include mk src tests tools
# .buildinfo makes a build from the tarball (no git) report the commit it was cut from
dist-hook:
	$(SHELL) $(top_srcdir)/tools/buildinfo.sh --srcdir='$(abs_top_srcdir)' \
		--version='$(PACKAGE_VERSION)' --dist-info='$(distdir)/.buildinfo'
	-find '$(distdir)' \( -name .deps -o -name '*.o' -o -name '*.a' -o -name .dirstamp \) \
		-exec rm -rf {} + 2>/dev/null
