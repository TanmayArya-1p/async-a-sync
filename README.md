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
(cd vendor/fil-c-src && git sparse-checkout set filc libpas llvm/lib/Transforms \
  llvm/include/llvm/Transforms clang/lib/CodeGen)
```

Build, then run the demos:

```sh
./runtime/build.sh          # the io_uring runtime
./compiler/build.sh         # the patched clang; the long step

# there are 4 demos in this repo
make demo-wordcount         # this has the timing measurements
make demo-plain             # a simple read loop with no submit/wait calls
make demo-provenance        # dependency annotation of async function calls

./tests/run.sh              # full test suite, all four demos included
```

## The demos

| demo | what it shows |
|---|---|
| `demo_plain_io` | the ergonomics case: request every file, count every file, no marker anywhere |
| `demo_wordcount` | one word count program ran twice, blocking and implicit, over 512 files |
| `demo_provenance` | a write and a read on two descriptors of one file, ordered by a provenance token |
| `demo_async_io` | lazy resolution, blocking vs issuing-all, the dependency DAG |


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
- **The device is not fully saturated.** The implicit path reaches ~52 kIOPS where plain threads sustain ~184 kIOPS; the gap is an `io-wq` worker ceiling which must be tuned for the specific workload.
