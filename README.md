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
  `filc_async_wait_all(tags, count)` joins completion pointers when a later
  call needs to wait for several operations. See the
  [group contract](wiki/Framework-API.md#joining-pending-buffers).
- **The first access waits.** Fil-C's
  [InvisiCaps](https://fil-c.org/invisicaps) give every pointer a reference to
  its object's header. The patched compiler tests a pending flag there before
  each access, so the first access to a pending buffer waits for its call, at
  the access site itself.
- **Pluggable runtimes.** The framework does not depend on any runtime. Each
  annotated function names the runtime that runs it (`runtime=<name>`), and
  one program can link several. This repository ships two: one turns calls
  into io_uring requests and sends them to the kernel in one batch, the first
  time a result is needed; the other sends calls to a TCP server, for the rpc
  demos.

```c
FILC_ASYNC(io_uring, FILC_OP(pread), FILC_BOUT(buf))
void* read_at(int fd, void* buf, size_t len, unsigned long offset);

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

The [wait-all demo](demos/pragma/demo_pragma_wait-all.c) queues 30 reads, joins
their buffers with `filc_async_wait_all`, and hands off one write that waits for
all of them.
Run it with `make demo-pragma-wait-all` after building the patched compiler.

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

./runtime/build.sh           # the framework and the runtimes
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
| **Reference** | [Annotations](wiki/Annotation-Reference.md) · [Runtime API](wiki/Runtime-API.md) · [Framework API](wiki/Framework-API.md) · [io_uring runtime](wiki/io_uring-Runtime.md) · [RPC runtime example](wiki/RPC-Runtime.md) · [Explicit API](wiki/Explicit-API.md) · [Tests](wiki/Testing.md) |
| **Explanation** | [Architecture](wiki/Architecture.md) · [Demos and performance](wiki/Performance.md) · [Comparisons](wiki/Comparisons.md) · [Limitations](wiki/Limitations.md) · [Glossary](wiki/Glossary.md) |

## Layout

| Path | What it is |
|---|---|
| `compiler/` | the FilAsync pass, and patches to Fil-C's clang for the pass and the access hook |
| `runtime/` | `include/` the public headers, `framework/` the async framework, `io_uring/` and `rpc/` the runtimes, `patches/` Fil-C's forwarder generator |
| `demos/` | the demos (`make help`): `pragma/` annotated calls, `explicit/` the `fasync_*` API, `wordcount/` one program built three ways, `rpc/` calls to a TCP server through `runtime=rpc`, `comparison/` the same task with an annotated call and the usual way, `common/` shared helpers |
| `tests/` | the test suite (`./tests/run.sh`) |
| `scripts/` | build helpers shared by `compiler/` and `runtime/` |
| `wiki/` | the documentation |
