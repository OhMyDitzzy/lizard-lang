# Building on Android (Termux)

## Native Termux

```
pkg install clang make autoconf automake git
./autogen.sh && ./configure && make && make check
```

or, without Autotools: `pkg install clang` and `sh tools/build-plain.sh check` ([plain.md](plain.md)).
Termux is aarch64 on Linux (bionic), so the JIT is enabled. Install into the prefix of Termux with
`./configure --prefix=$PREFIX`.

## proot-distro

`proot-distro login debian` gives a normal Debian userland; build as in [prerequisites.md](prerequisites.md).

## AddressSanitizer does not work in proot

ASan and LSan fail at start-up inside proot (the kernel's `vm.mmap_rnd_bits` cannot be set, or ptrace is
refused), so `./configure --enable-sanitizers` builds but the programs abort. Alternatives:

* `./configure --enable-poison --with-build-type=debug`, then `make check`: freed objects are poisoned and quarantined,
  which finds use-after-free ([MEMORY.md](../MEMORY.md)).
* `make test_links fuzz_links && ./fuzz_links 1000` (the model fuzzer).
* Let CI run the sanitizers: `.github/workflows/sanitizers.yml` runs ASan, UBSan, poison and Valgrind on every push.
