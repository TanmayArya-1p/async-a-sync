# Build and link

This guide covers building the patched compiler and the libraries, and
linking programs against them. For a first-time walkthrough, see
[Getting started](Getting-Started.md).

## Requirements

| For | You need |
|---|---|
| everything | Linux x86-64; the Fil-C 0.685 distribution and a `deluge` source checkout under `vendor/` (see [Getting started](Getting-Started.md#1-fetch-fil-c)) |
| `runtime/build.sh` | Ruby, a host Clang, Linux headers |
| `compiler/build.sh` | CMake, Ninja, a host C++ compiler, Git |
| running anything that submits requests | a kernel and sandbox that allow `io_uring_setup` |

## Pin the Fil-C source revision

`deluge` is a moving branch. Two things are tied to the source shape at
`d80c8bba1c58`:

- the compiler overrides in `compiler/upstream-overrides/`;
- `upstream-patches/sroa-release-verbose.patch`.

Build from that revision unless you intend to rebase them. At that revision,
both compilers report Fil-C 0.685 and Clang 20.1.8. A shallower sparse
checkout is enough for the runtime, but not for a complete Clang build.

## Build the libraries

```sh
./runtime/build.sh
```

The script:

- regenerates Fil-C's native forwarders to include the io_uring syscalls and
  the framework's native helpers;
- copies the distribution's `libpizlo.a` into
  `runtime/build/lib/libpizlo.a`, adding the framework and native bridges;
- archives the io_uring runtime as `runtime/build/lib/libfilc_async_uring.a`.

| Archive | Contents |
|---|---|
| `libpizlo.a` | Fil-C's runtime; the framework (`filc_async.c`, `filc_async_arena.c`); the pending-flag native helper (`filc_async_native.c`); the io_uring syscall bridge (`fasync_native.c`) |
| `libfilc_async_uring.a` | the io_uring runtime (`filc_async_uring.c`) and its request layer (`fasync*.c`) |

## Build the compiler

```sh
JOBS=8 ./compiler/build.sh
```

The script:

- installs the repository's overrides into the source checkout
  (`FilAsync.cpp`, `FilPizlonator.cpp`, the pass registration);
- applies the SROA patch, once;
- builds Clang into `vendor/fil-c-src/build/bin/filcc`.

The SROA patch fixes a Release-build error where a log statement reads a
field that is compiled out under `NDEBUG`.

- **Memory.** The build has thousands of steps and needs a lot of RAM. On a
  13 GiB machine, 16 jobs got a compiler process killed, while 8 finished.
  Set `JOBS` to fit your memory. Ninja reuses finished objects after a
  restart.
- **Linker.** Clang is linked with lld when the host can use it, and with the
  default linker otherwise. Set `LLVM_ENABLE_LLD=ON` or `OFF` to force a
  choice. The setting only applies when the build directory is first
  configured.
- **pizfix.** The source-built driver looks for `pizfix` at
  `vendor/fil-c-src/pizfix`. `compiler/build.sh` and `tests/run.sh` link the
  distribution's copy there when it is missing. To make the link by hand:

  ```sh
  ln -sfn "$PWD/vendor/filc-0.685-linux-x86_64/pizfix" vendor/fil-c-src/pizfix
  ```

## Link a program

Put the libraries after the program's sources or objects:

```sh
vendor/fil-c-src/build/bin/filcc -O2 -static \
  -Werror=pragma-clang-attribute -Iruntime/src -Lruntime/build/lib \
  -o app app.c -lpizlo -lfilc_async_uring -lpizlo -lc
```

- **`-lpizlo` twice.** The framework (in `libpizlo`) and the runtime refer to
  each other, so `libpizlo` is named on both sides of the runtime.
- **The trailing `-lc`.** The framework and the runtime use Fil-C's pthreads.
  Without `-lc`, the link fails with undefined `pizlonated_pthread_mutex_lock`
  and similar symbols.
- **`-Werror=pragma-clang-attribute`.** Turns a pragma placed around a call
  (which does nothing) into an error.
- **`-DFASYNC_COMPILER_INSERTS_CHECKS`.** Makes `FASYNC_ACCESS` a no-op. Use
  it with the patched compiler when you use the explicit API.

**Another runtime.** Replace `-lfilc_async_uring` with your runtime's objects
or library. See [Write a runtime](Writing-a-Runtime.md#5-link-and-test).

**Stock `filcc`.** The stock `filcc` from the distribution can build and link
programs that use only the explicit API. It does not rewrite annotated calls
or insert the pending-flag test.

## Run the tests and demos

```sh
./tests/run.sh
make help                  # lists the demo targets
make demo-pragma           # every annotated-call demo
make all-demos             # the four explicit-API demos
```

`tests/run.sh` skips the patched-compiler tests when
`vendor/fil-c-src/build/bin/filcc` is missing. Read the summary line for
skips as well as failures. See [Tests](Testing.md).
