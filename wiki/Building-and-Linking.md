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

`deluge` is a moving branch. The patches in `compiler/patches/` and
`runtime/patches/` are diffs against the source at `d80c8bba1c58`. Build
from that revision unless you intend to rebase them. At that revision,
both compilers report Fil-C 0.685 and Clang 20.1.8. A shallower sparse
checkout is enough for the runtime, but not for a complete Clang build.

## Build the libraries

```sh
./runtime/build.sh
```

The script:

- applies `runtime/patches/libpas-forwarders.patch` to Fil-C's forwarder
  generator, and regenerates the native forwarders, which then include the
  io_uring syscalls and the framework's native helpers;
- copies the distribution's `libpizlo.a` into
  `runtime/build/lib/libpizlo.a`, adding the framework and native bridges;
- archives each runtime as a library of its own.

| Archive | Built from | Contents |
|---|---|---|
| `libpizlo.a` | `runtime/framework/`, `runtime/io_uring/fasync_native.c` | Fil-C's runtime; the framework (`filc_async.c`, `filc_async_arena.c`); the pending-flag native helper (`filc_async_native.c`); the io_uring syscall bridge (`fasync_native.c`) |
| `libfilc_async_uring.a` | `runtime/io_uring/` | `runtime=io_uring` (`filc_async_uring.c`) and its request layer (`fasync*.c`) |
| `libfilc_async_rpc.a` | `runtime/rpc/` | `runtime=rpc` (`rpc_runtime.c`) |

**Why io_uring's native half is in `libpizlo.a`.** Fil-C's generated
forwarders, which live in `libpizlo.a`, call every native entry point, so the
natives have to be there too. A program that links no io_uring runtime still
carries them, unused.

Programs include the public headers with `-I runtime/include`.

## Build the compiler

```sh
JOBS=8 ./compiler/build.sh
```

The script:

- copies the FilAsync pass (`compiler/pass/`) into the source checkout;
- applies each patch in `compiler/patches/`, once;
- builds Clang into `vendor/fil-c-src/build/bin/filcc`.

The patches:

- **`backend-util-run-filasync.patch`** runs FilAsync first in Fil-C's
  pipeline, and **`instrumentation-cmake-filasync.patch`** builds it.
- **`filpizlonator-pending-hook.patch`** adds the pending-flag test that
  FilPizlonator puts before each access.
- **`filc-async-param-names.patch`** makes clang record the parameter names
  of each `filc_async` function, which the options refer to, and which of
  its parameters point to a `const` type, which makes them inputs.
- **`sroa-release-verbose.patch`** fixes a Release-build error where a log
  statement reads a field that is compiled out under `NDEBUG`.

**Applying them.** Each patch holds only our change. `scripts/apply_filc_patches.sh`
applies a patch once, and leaves an already-patched file alone so it is not
rebuilt. If a file holds something else, such as a whole-file copy an older
version of this repository installed, it first restores the upstream file.

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
  -Iruntime/include -Lruntime/build/lib \
  -o app app.c -lfilc_async_uring -lpizlo -lc
```

- **Runtimes before `-lpizlo`.** Link every runtime the program's
  annotations name (`runtime=io_uring` is `-lfilc_async_uring`). A runtime
  refers to the framework in `libpizlo`, but the framework never refers to a
  runtime, so the runtimes come first.
- **The trailing `-lc`.** The framework and the runtime use Fil-C's pthreads.
  Without `-lc`, the link fails with undefined `pizlonated_pthread_mutex_lock`
  and similar symbols.
- **`-Werror=pragma-clang-attribute`.** Only for annotations written with
  `#pragma clang attribute`. It turns a pragma placed around a call (which does
  nothing) into an error. `FILC_ASYNC` cannot be misplaced that way.
- **`-DFASYNC_COMPILER_INSERTS_CHECKS`.** Makes `FASYNC_ACCESS` a no-op. Use
  it with the patched compiler when you use the explicit API.

**Other runtimes.** Add your runtime's objects or library next to
`-lfilc_async_uring`, or in place of it if nothing names `runtime=io_uring`.
See [Write a runtime](Writing-a-Runtime.md#5-link-and-test).

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
