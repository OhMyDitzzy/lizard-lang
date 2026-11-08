# Troubleshooting

| Message | Cause and fix |
| --- | --- |
| `./configure: No such file or directory` | you are in a git checkout: run `./autogen.sh` first (a tarball has `configure`) |
| `aclocal: command not found`, `autoreconf: command not found` | install `autoconf` and `automake` ([prerequisites.md](prerequisites.md)), or use a release tarball, or [plain.md](plain.md) |
| `automake: error: ... Automake 1.16 ... required` or `version mismatch` | automake older than 1.16: install a newer one, or build from a tarball |
| `configure: error: a C11 compiler is required` | the compiler does not accept `-std=c11`: install a newer gcc or clang, or set `CC=` |
| `configure: error: the math library (-lm) is needed` | the C library has no `fmod` in `-lm`: install the development package of your libc |
| `configure: error: --enable-sanitizers and --enable-static-binary cannot be combined` | pick one |
| `configure: error: --enable-sanitizers: ... cannot build with ASan and UBSan here` | the compiler lacks the sanitizer runtime (install `libasan`/`libubsan`, or see [termux.md](termux.md)) |
| `configure: error: An official build needs a clean tree` | commit your changes, or configure without `--enable-official` |
| `configure: error: --enable-official needs --with-signing-key=FPR` | give the fingerprint |
| `make: *** No rule to make target 'mk/tests.am'` or `mk/tests.am is stale` | run `./autogen.sh` again after editing `tests/tests.list` |
| `Clock skew detected` | file times are in the future (unpacked from an archive): `find . -exec touch {} +` |
| `make check` prints "skipped" | `--disable-tests` or a cross build, by design |
| `skink_cli` is SKIP | this build has no JIT; the test needs it |
| `lizard --version` says `dirty` | you have uncommitted or untracked files; `git status` shows which |
| plain build: `'ar' not found` | install binutils (`pkg install binutils` on Termux) or set `AR=` |
