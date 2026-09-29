# async-a-sync

**Write blocking-looking synchronous C that runs asynchronously**


async-a-sync is a compiler and runtime system built on top of [Fil-C](https://github.com/pizlonator/fil-c) that brings zero-syntax implicit asynchronous futures to C. It allows developers to write ordinary, blocking-looking C code that is fully asynchronously underneath at runtime.

It transforms blocking function calls into asynchronous backend submissions that return immediately. Values mutated by these operations carry provenance tags that propagate through pointer arithmetic (This is already provided by Fil-C). By extending Fil-C's [InvisiCaps](https://fil-c.org/invisicaps), our compiler automatically inserts lazy resolution barriers before memory accesses to results of async functions that resolve any pending values transparently at the access site itself.

Request batching is deferred until demand, and resolution polls completion queues directly in userspace memory. The asynchronous backend interface is supposed to be generic across arbitrary asynchronous backends. In this repo we provide an io_uring driver demonstrating substantial ergonomic and throughput gains on system call workloads.

**The crux is a single synchronous-looking loop as follows:**

```c
// request every file; the requests only enqueue and nothing blocks or context switches
for (int i = 0; i < n; i++)
  fasync_pread(fd[i], buf[i], len, 0);

// count every file; the first buffer access is where the queue reaches
// the kernel lazily and as one batch. No submit or wait call appears here
for (int i = 0; i < n; i++)
  if (count_words(buf[i]) != expect[i])
    return 1;

// The same code compiled in a normal synchronous model would block in the first loop until it has read every file.
```

## Quickstart

Prerequisites are a Fil-C 0.685 distribution and a Fil-C source checkout, both
under `vendor/`:

```sh
# the filc compiler, its runtime, and pizfix.
mkdir -p vendor && cd vendor
curl -LO https://github.com/pizlonator/fil-c/releases/download/v0.685/filc-0.685-linux-x86_64.tar.xz
tar xf filc-0.685-linux-x86_64.tar.xz && cd ..

# filc sources that we need to modify in this repo for building
git clone --depth 1 --filter=blob:none --sparse \
  -b deluge https://github.com/pizlonator/fil-c.git vendor/fil-c-src
(cd vendor/fil-c-src && git sparse-checkout set \
  clang cmake filc libpas lld llvm third-party &&
  git fetch --depth 1 --filter=blob:none origin d80c8bba1c58f68c33b0ed5e71113c44354f5bb8 &&
  git checkout FETCH_HEAD)   # the revision the compiler patches are tested on
```

Build, then run the demos:

```sh
./runtime/build.sh          # the io_uring runtime
./compiler/build.sh         # the patched clang; the long step

# there are 4 demos in this repo
make demo-wordcount         # this has the timing measurements
make demo-plain             # a simple read loop with no submit/wait calls
make demo-provenance        # dependency annotation of async function calls
make demo-async             # lazy resolution, blocking vs issuing-all, the DAG

./tests/run.sh              # full test suite, all four demos included
```

Everything runs on Linux x86-64, and the runtime needs io_uring. Inside Docker
that means `--security-opt seccomp=unconfined`, because the default seccomp
profile blocks it. An x86-64 container on Apple silicon runs under Rosetta,
which does not implement io_uring at all. Without io_uring, `tests/run.sh`
skips the tests that submit requests and still runs the rest.

See [compiler/README.md](compiler/README.md) for build requirements, the
patched compiler's runtime path, link flags, and common failure signatures.
See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for the compiler-to-runtime
implementation, ABI, dependency ordering, test map, and current limits.

`tests/check_forwarders.sh` can run without Fil-C. The full suite also checks
the generated forwarders, the symbols in a linked two-file executable, the
io_uring backend through direct `filc_async_submit` calls, and the annotated
compiler-to-runtime path.

## The demos

| demo | what it shows |
|---|---|
| `demo_plain_io` | the ergonomics case: request every file, count every file, no marker anywhere |
| `demo_wordcount` | one word count program ran twice, blocking and implicit, over 512 files |
| `demo_provenance` | a write and a read on two descriptors of one file, ordered by a provenance token |
| `demo_async_io` | lazy resolution, blocking vs issuing-all, the dependency DAG |

## Annotated io_uring calls

The `filc_async` annotation path also submits `op=pread`, `op=pwrite`,
`op=openat`, `op=fsync`, and `op=close` to the same io_uring runtime. Annotated
functions use the usual syscall argument order: `(fd, buffer, length, offset)`
for reads and writes, `(dirfd, path, flags, mode)` for open, and `(fd)` for
sync and close. Mark output buffers with `bout=`, input buffers and paths with
`bin=`, and descriptor arguments with `fd=`. The io_uring runtime runs the
function body before it issues the request, so the body can log or instrument
the call; the call returns a task pointer for `filc_async_poll` or
`filc_async_wait`. A read's output buffer can also resolve on its first access.
The compiler currently rewrites pointer-returning and void call sites; scalar
returning call sites remain direct calls.

Like `op=`, `fd=`, and buffer options, dependency options can appear on a
function declaration or definition. When both carry a `filc_async` annotation,
the definition's options apply. Repeat
`r_dep=<argument index>` or `w_dep=<argument index>` to list resources the call
reads or writes. For example, a write declared with `"w_dep=0"` uses its fd as
the dependency key, and a read with `"r_dep=0", "w_dep=1"` reads that fd resource
and writes its output buffer. Scalar arguments with the same value share a key;
pointer arguments into the same object share a key. A `:<name>` suffix, as in
`"w_dep=0:meta"`, puts the key in a namespace: equal values in different
namespaces are different resources, and a key without a name is in a namespace
of its own. Two dependency options on one argument must agree in mode and name. Calls with a matching key
dispatch in call order whenever either side writes (read/write, write/read, or
write/write). Two reads can be in flight together. Each call goes through a
stub the compiler emits for its function, which locks every dependency
argument's value before the call reaches the runtime: a read lock is shared, a
write lock is exclusive, and a conflicting call waits in its stub until the
holder completes and releases the lock.
When compiling annotated code, use `-Werror=pragma-clang-attribute` so a
pragma placed around a call is a compiler error.

```c
#pragma clang attribute push(__attribute__((annotate("filc_async", "op=pwrite", "fd=0", "bin=1", "w_dep=0"))), apply_to=function)
void* async_pwrite(int fd, const void* buf, size_t len, unsigned long offset);
#pragma clang attribute pop
```

`make demo-pragma` builds the demos of annotated calls and runs them in order;
`make demo-pragma-<name>` runs one, and `ARGS=...` passes it arguments. Each
prints a table, a `=>` result line and `DEMO OK` when its checks pass. A
demo's source holds only the code it demonstrates; the setup, timing, tables
and checks are in `demos/pragma_report.hh`.

| Demo | Shows |
|---|---|
| `hello` | one annotated `pread`: the call marks the buffer, the runtime runs the body and queues a request, and the first read of the buffer sends it |
| `lifecycle` | `openat`, `pwrite`, `fsync`, `pread`, `close` issued back to back and kept in order by `w_dep`/`r_dep` |
| `ordering` | conflicting calls on one fd wait for each other; independent calls batch into one kernel submit |
| `coldread` | the same loop calling `pread` and `async_pread` over 512 cold files, next to hand-written `fasync_*` |
| `scaling` | that comparison for 1 to 2048 files, with the time spent inside each annotated call |
| `overlap` | reading and hashing 256 files, with the reads and the hashing also timed alone |

On a Linux machine with an NVMe SSD, `coldread` is about 2.7x faster than
blocking `pread` and as fast as the hand-written `fasync_*` version, and
`overlap` is about 1.8x faster. In `scaling` an annotated call costs 1-2 us
with up to 1024 calls in flight. At 2048 files the gain is lower and varies
more between runs: calls wait for room in the 1024-entry request table. The
numbers depend on the storage device and on other I/O on the machine. Each
timing demo first checks that dropping the page cache makes reads slower, and
warns if it does not, because then there is no device latency to overlap.


## Results

Measured outputs from this repo's own programs. timings vary by machine.

`demos/run_wordcount.sh` builds one synchronous looking word-count C program two ways and times both
over 512 files with the page cache dropped.

```
  sync        25.34 ms  (338154 words)
  implicit    13.80 ms  (338154 words)
  implicit finish in 1.84x the time
```

**Throughput regime:** `tests/stage7_throughput.c`, 20,000 reads of 64 bytes
from a warm page cache. Blocking code pays one kernel entry per read; the
implicit program pays 1302 for all 20 000, submitted 200 per batch, with the
completion ring polled in userspace:

```
  blocking:  14.98 ms  (20000 kernel entries, one per read)
  async:     13.61 ms  (1302 kernel entries: 100 submits + 1202 waits)
  entries saved: 15.4x         observed wall clock: 1.10x
```

**Latency regime:** `tests/stage8_latency.c`, 512 files of 4 KiB, page cache
dropped before each pass. Three arms read the same bytes: `A` blocks per file, `B`
uses io_uring with an explicit wait, `C` is the implicit program. `C` is 2.71x
faster than `A` and 1.22x faster than `B`, because its first access publishes the
whole batch lazily rather than submitting eagerly:

```
  cache dropped:  22.37 ms  (43.68 us/file; 11x cold vs warm)
  C vs A (implicit vs blocking):            2.71x
  C vs B (implicit vs explicit io_uring):   1.22x
  all three arms read the same bytes, the hook resolved C's buffers
```


## Layout

| Path | What it is |
|---|---|
| `runtime/` | io_uring driver, lazy resolution, provenance, dependency DAG |
| `compiler/` | the modified FilPizlonator pass that inserts the resolution hook |
| `demos/` | the showcase (`make demo-*`) |
| `tests/` | the test suite (`./tests/run.sh`) |


## Caveats

There are a few caveats and drawbacks that are yet to be addressed. We hope to flesh this out in the future.

- **Fil-C's Overhead** Fil-C is documented at 1.5-4x slower than gcc. Thus the gains must be compensated by the asynchronous nature of the workload.
- **Hidden dependencies must be annotated** invisible sharing is declared with an effect set or a token.
- **Supported calls are a subset.** `pread`, `pwrite`, `openat`, `close`, `fsync`. Plain `read` is out because it has no offset. Very few syscalls are supported by `io_uring`, which limited our work.
- **Speedup scope is limited right now:** Significant speedup is only observed in reads that dont read from page cache (via `O_DIRECT`).
- **Single-threaded runtime.** One io_uring ring serves the whole process, without locks. The thread that makes the first request owns it; a request or wait from any other thread stops the program with a clear message. Other threads may run and compute, but must not read a buffer while its request is in flight: the owner touches it or waits on it first.
- **The device is not fully saturated.** The implicit path reaches ~52 kIOPS where plain threads sustain ~184 kIOPS; the gap is an `io-wq` worker ceiling which must be tuned for the specific workload.
