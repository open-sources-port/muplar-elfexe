Rosetta x86_64 test fixtures vendored for self-contained matrix coverage.

- `x86_64-rosetta-audit`
  - static x86_64 Linux ELF built from `tests/x86_64-rosetta-audit.c`
- `x86_64-rosetta-tls0`
  - static x86_64 Linux ELF built from `tests/x86_64-rosetta-tls0.c`
- `x86_64-rosetta-madvise`
  - static x86_64 Linux ELF built from `tests/x86_64-rosetta-madvise.c`
  - used by `tests/test-rosetta-madvise.sh`
- `x86_64-rosetta-msync`
  - static x86_64 Linux ELF built from `tests/x86_64-rosetta-msync.c`
  - used by `tests/test-rosetta-msync.sh`
- `x86_64-rosetta-mremap`
  - static x86_64 Linux ELF built from `tests/x86_64-rosetta-mremap.c`
  - used by `tests/test-rosetta-mremap.sh`
- `x86_64-glibc-rootfs.tar.gz`
  - minimal x86_64 glibc rootfs used by `tests/test-rosetta-glibc.sh`
  - contains `hello-dynamic`, `dlopen-probe`, `tls-probe`,
    `gdtls-probe`, `pthread-tls-probe`, the glibc loader, `libc.so.6`,
    `libm.so.6`, and `libgdtls.so`
  - `hello-dynamic`     built from `tests/x86_64-glibc-hello.c`
  - `dlopen-probe`      built from `tests/x86_64-glibc-dlopen.c`
  - `tls-probe`         built from `tests/x86_64-glibc-tls.c`
  - `gdtls-probe`       built from `tests/x86_64-glibc-gdtls.c`
  - `libgdtls.so`       built from `tests/x86_64-glibc-gdtls-lib.c`
  - `pthread-tls-probe` built from `tests/x86_64-glibc-pthread-tls.c`

These fixtures exist so `make test-rosetta-all` and
`bash tests/test-matrix.sh elfuse-x86_64` do not require a private build host,
`ld.lld`, or an ad hoc local cross-toolchain.

The cited `tests/*.c` sources are not wired into any in-tree build rule
(the elfuse Makefile builds aarch64 host binaries; these fixtures are
x86_64 Linux ELFs). When one of them changes, the binary has to be
rebuilt out of tree on an x86_64 Linux host and the result re-vendored
here. Rough recipe:

```
# On an x86_64 Linux host with gcc + the matching glibc dev headers:
gcc -O2 -o hello-dynamic         tests/x86_64-glibc-hello.c
gcc -O2 -ldl -o dlopen-probe     tests/x86_64-glibc-dlopen.c
gcc -O2 -o tls-probe             tests/x86_64-glibc-tls.c
gcc -O2 -fPIC -shared -o libgdtls.so tests/x86_64-glibc-gdtls-lib.c
gcc -O2 -ldl -o gdtls-probe      tests/x86_64-glibc-gdtls.c
gcc -O2 -pthread -o pthread-tls-probe tests/x86_64-glibc-pthread-tls.c
gcc -O2 -static -o x86_64-rosetta-audit   tests/x86_64-rosetta-audit.c
gcc -O2 -static -o x86_64-rosetta-tls0    tests/x86_64-rosetta-tls0.c
gcc -O2 -static -o x86_64-rosetta-madvise tests/x86_64-rosetta-madvise.c
gcc -O2 -static -o x86_64-rosetta-msync   tests/x86_64-rosetta-msync.c
gcc -O2 -static -o x86_64-rosetta-mremap  tests/x86_64-rosetta-mremap.c
# Stage the matching ld.so / libc.so.6 / libm.so.6 from the same host
# into a rootfs/ tree alongside libgdtls.so under lib/x86_64-linux-gnu/,
# then tar -czf x86_64-glibc-rootfs.tar.gz rootfs/.
```

The two static audit fixtures and the rootfs tarball then drop into
this directory verbatim.
