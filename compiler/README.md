# Building and validating the patched Fil-C compiler

This repository needs two Fil-C artifacts. The v0.685 distribution supplies
`pizfix` and the stock `filcc` used to build the runtime. A `deluge` source
checkout supplies the Clang sources that `compiler/build.sh` patches and builds.
The full test suite uses both compilers.
For the compiler pass, runtime ABI, and dependency implementation, see
[the architecture guide](../docs/ARCHITECTURE.md).

## Set up the sources

From the repository root:

```sh
mkdir -p vendor
curl -L -o vendor/filc-0.685-linux-x86_64.tar.xz \
  https://github.com/pizlonator/fil-c/releases/download/v0.685/filc-0.685-linux-x86_64.tar.xz
tar -C vendor -xf vendor/filc-0.685-linux-x86_64.tar.xz
git clone --depth 1 --filter=blob:none --sparse -b deluge \
  https://github.com/pizlonator/fil-c.git vendor/fil-c-src
git -C vendor/fil-c-src sparse-checkout set \
  clang cmake filc libpas lld llvm third-party
# pin the revision the overrides and the SROA patch were tested against
git -C vendor/fil-c-src fetch --depth 1 --filter=blob:none origin \
  d80c8bba1c58f68c33b0ed5e71113c44354f5bb8
git -C vendor/fil-c-src checkout FETCH_HEAD
```

The smaller sparse checkout in older setup instructions is enough for the
runtime but not for a complete Clang build. `deluge` is a moving branch; the
overrides and `upstream-patches/sroa-release-verbose.patch` are tied to the
source shape at `d80c8bba1c58`, so build from that revision
unless you mean to rebase them. Both compilers at that revision report
Fil-C 0.685 and Clang 20.1.8.

## Build and test

```sh
./runtime/build.sh
JOBS=8 ./compiler/build.sh
./tests/run.sh
```

The compiler build needs CMake, Ninja, a host C++ compiler, Git, and the source
checkout above. The runtime build also needs Ruby, a host Clang, and Linux
headers. `compiler/build.sh` installs the repository's upstream overrides,
applies `upstream-patches/sroa-release-verbose.patch` to the source checkout,
and creates `vendor/fil-c-src/build/bin/filcc` after Clang links. The SROA
patch fixes a Release build error where a log statement reads a field omitted
under `NDEBUG`. The build script applies it only once.

`compiler/build.sh` links Clang with lld when the host compiler can link with
it and otherwise falls back to the host's default linker; set
`LLVM_ENABLE_LLD=ON` or `OFF` to force either. The choice only applies when
the build directory is first configured.

The Clang build has thousands of compilation steps and uses substantial RAM.
On a 13 GiB machine, 16 jobs caused the OS to kill a compiler process; eight
jobs completed the memory-heavy Clang phase. Ninja reuses completed objects
after a restart. Use `JOBS` to match available memory.

The source-built driver looks for `pizfix` at
`vendor/fil-c-src/pizfix`. `compiler/build.sh` and `tests/run.sh` both link
the distribution's `pizfix` there when it is missing. To make the link by
hand:

```sh
ln -sfn "$PWD/vendor/filc-0.685-linux-x86_64/pizfix" vendor/fil-c-src/pizfix
```

To link a program against the extended runtime, put the libraries after the
source or object files:

```sh
vendor/fil-c-src/build/bin/filcc -O2 -static \
  -Werror=pragma-clang-attribute -Iruntime/src -Lruntime/build/lib \
  -o app app.c -lpizlo -lc
```

The driver itself appends `-lpizlo -lc -lpizlo`, so the runtime's objects are
pulled in after its libc scan. Earlier versions of the runtime called Fil-C's
pthread mutex functions and failed to link without the trailing `-lc`
(undefined `pizlonated_pthread_mutex_lock` and `_unlock`). The only libc
symbol the runtime still uses is errno's `__errno_location`, which virtually
every program already pulls in, so the trailing `-lc` is now a safeguard; the
repository's scripts keep it.

## ABI and test boundaries

`compiler/upstream-overrides/llvm/lib/Transforms/Instrumentation/FilAsync.cpp`
stages each annotated argument in a 16-byte cell. Scalar values occupy its
first word; pointer values retain their Fil-C capability. The C language's
`sizeof(void*)` is still 8 here, so a plain pointer/word union is only 8 bytes
and cannot be used to index the staged array. Keep the 16-byte layout in
`runtime/src/filc_async.c` and the direct-submit fixture
`tests/t_backend_io_uring.c` in sync with the pass. An 8-byte stride made a
small read length appear larger than `UINT_MAX`, returning `-EOVERFLOW` before
an SQE was queued.

`tests/run.sh` runs host checks, stock Fil-C runtime tests, and patched
compiler tests. The latter are skipped when the patched binary is absent, so
check the summary and the skip message. A full run on Linux 7.0, x86-64,
with io_uring available passes with no failures or skips.

| Symptom | Check |
|---|---|
| Missing `crtbegin.o`, `filc_crt.o`, or `-lyolort` | Check `vendor/fil-c-src/pizfix` points to the distribution's `pizfix`. |
| Undefined `pizlonated_pthread_mutex_*` | An older runtime that still used a mutex; rebuild it with `./runtime/build.sh`, or put `-lpizlo -lc` after program objects. |
| Annotated request returns `-75` with no SQE | Check the 16-byte staged argument stride. `-75` is `EOVERFLOW`. |
| `io_uring_setup: Operation not permitted` | The execution environment blocks io_uring; run the suite where that syscall is allowed. |
| `SROA.cpp`: `AI` undeclared in Release | Ensure `compiler/build.sh` applied the SROA patch to the source checkout. |

For the annotation syntax and dependency ordering rules, see the root
`README.md`. The ordinary implementation of an annotated function remains a
linker symbol; this io_uring backend does not call its body. The linked
two-file test in `tests/t_linked_async_main.c` and
`tests/t_linked_async_def.c` verifies that contract.

| Area | Starting point |
|---|---|
| Call rewriting and argument metadata | `compiler/upstream-overrides/llvm/lib/Transforms/Instrumentation/FilAsync.cpp` |
| Lazy memory-access resolution hook | `compiler/upstream-overrides/llvm/lib/Transforms/Instrumentation/FilPizlonator.cpp` |
| Annotated task dispatch and dependency claims | `runtime/src/filc_async.c` |
| io_uring requests and native syscall bridge | `runtime/src/fasync.c`, `runtime/src/fasync_native.c` |
| Link and dependency verification | `tests/check_linkage.sh`, `tests/check_dependency_options.sh`, `tests/t_pragma_dependencies.c` |
