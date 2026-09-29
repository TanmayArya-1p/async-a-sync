# async-a-sync

**Write blocking-looking synchronous C that runs asynchronously**

async-a-sync is a compiler and runtime system built on top of
[Fil-C](https://github.com/pizlonator/fil-c). It brings zero-syntax implicit
asynchronous futures to C: you write ordinary, blocking-looking code, and it
runs asynchronously underneath.

**How it works:**

- **Annotated calls return at once.** A call to an annotated function is handed
  to an asynchronous runtime and returns immediately.
- **Buffers stay pending.** The buffers the call produces are pending until it
  completes.
- **The first access waits.** Fil-C's
  [InvisiCaps](https://fil-c.org/invisicaps) give every pointer a reference to
  its object's header. The patched compiler tests a pending flag there before
  each access, so the first access to a pending buffer waits for its call, at
  the access site itself.
- **Pluggable runtimes.** The framework does not depend on any runtime. This
  repository ships one that turns calls into io_uring requests and sends
  them to the kernel in one batch, the first time a result is needed.

```c
#pragma clang attribute push(__attribute__((annotate("filc_async", "op=pread", "fd=0", "bout=1"))), apply_to=function)
void* read_at(int fd, void* buf, size_t len, unsigned long offset);
#pragma clang attribute pop

// request every file: each call only queues a request; nothing blocks
for (int i = 0; i < n; i++)
  read_at(fd[i], buf[i], len, 0);

// count every file: the first access sends the whole batch to the kernel
// and waits for that one buffer; no submit or wait call appears anywhere
for (int i = 0; i < n; i++)
  if (count_words(buf[i]) != expect[i])
    return 1;
```

Compiled normally, the first loop would block on every read in turn. Here,
512 cold-cache reads finish about 3.5x faster than blocking `pread`.

## Quickstart

On Linux x86-64 with io_uring available:

```sh
# Fil-C 0.685 and the source revision the compiler patches are tested on
mkdir -p vendor
curl -L https://github.com/pizlonator/fil-c/releases/download/v0.685/filc-0.685-linux-x86_64.tar.xz \
  | tar -C vendor -xJ
git clone --depth 1 --filter=blob:none --sparse -b deluge \
  https://github.com/pizlonator/fil-c.git vendor/fil-c-src
git -C vendor/fil-c-src sparse-checkout set clang cmake filc libpas lld llvm third-party
git -C vendor/fil-c-src fetch --depth 1 --filter=blob:none origin d80c8bba1c58f68c33b0ed5e71113c44354f5bb8
git -C vendor/fil-c-src checkout FETCH_HEAD

./runtime/build.sh           # the framework and the io_uring runtime
JOBS=8 ./compiler/build.sh   # the patched clang: the long step
./tests/run.sh               # the test suite
make demo-pragma             # the annotated-call demos
```

In Docker, run with `--security-opt seccomp=unconfined`. The default seccomp
profile blocks io_uring. Rosetta does not implement io_uring at all.

## Documentation

The documentation lives in [`wiki/`](wiki/Home.md):

| | |
|---|---|
| **Tutorial** | [Getting started](wiki/Getting-Started.md) |
| **How-to** | [Annotate a function](wiki/Annotating-Functions.md) · [Write a runtime](wiki/Writing-a-Runtime.md) · [Build and link](wiki/Building-and-Linking.md) · [Troubleshooting](wiki/Troubleshooting.md) |
| **Reference** | [Annotations](wiki/Annotation-Reference.md) · [Runtime API](wiki/Runtime-API.md) · [Framework API](wiki/Framework-API.md) · [io_uring runtime](wiki/io_uring-Runtime.md) · [Explicit API](wiki/Explicit-API.md) · [Tests](wiki/Testing.md) |
| **Explanation** | [Architecture](wiki/Architecture.md) · [Demos and performance](wiki/Performance.md) · [Limitations](wiki/Limitations.md) · [Glossary](wiki/Glossary.md) |

## Layout

| Path | What it is |
|---|---|
| `compiler/` | overrides and patches for Fil-C's clang: the FilAsync pass and the access hook |
| `runtime/` | the async framework, the runtime interface, and the io_uring runtime |
| `demos/` | the demos (`make help`) |
| `tests/` | the test suite (`./tests/run.sh`) |
| `wiki/` | the documentation |
