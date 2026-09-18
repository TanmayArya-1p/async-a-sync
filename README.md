# async-a-sync

**Write blocking-looking synchronous C that runs asynchronously**


A modified [Fil-C](https://github.com/pizlonator/fil-c) compiler turns certain function
calls into submissions for an asynchronous backend. Such function calls returns
immediately, and the first real use of its result resolves it transparently. The
method is generic and the backend is pluggable, so the same compiler mechanism
can drive a custom specialized backend for any function call. This repo ships
one backend, a small `io_uring` driver, and the demos use it to show the
syscall case, where the ergonomic and speedup gains are demonstrable.

The value such a call produces carries a tag (Provenance) naming its pending request, and any
pointer derived from it, by arithmetic, indexing, or field access, inherits that
tag. The first genuine access to a tagged pointer checks the completion queue;
if the request is not done yet, it spins, then parks. There is no submit or wait
call anywhere in the program; work reaches the backend lazily. The check is
emitted by the compiler alongside the bounds check Fil-C's
[InvisiCaps](https://fil-c.org/invisicaps) already places on every access.


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
./demos/run_wordcount.sh    # same source, sync vs implicit, timed
./tests/run.sh              # full suite, all four demos included
```

`compiler/build.sh` is a full clang build; lower `JOBS` if memory is tight.
Until that compiler exists the runtime still works, and `tests/run.sh` cleanly
skips the compiler-dependent stages. `run_wordcount.sh` wants a real disk,
because on tmpfs a read costs nothing and the ratio is meaningless. Set
`FILC_ROOT` and `FILC_SRC` explicitly if either checkout lives elsewhere.

## The demos

`demos/` is the showcase: one crux header (`demo_*.hh`) per file, driven by a
run file (`demo_*.c`).

| demo | what it shows |
|---|---|
| `demo_plain_io` | the ergonomic case: request every file, count every file, no marker anywhere |
| `demo_async_io` | lazy resolution, blocking vs issuing-all, the dependency DAG |
| `demo_wordcount` | one word count written twice, blocking and implicit, over 512 files |
| `demo_provenance` | a write and a read on two descriptors of one file, ordered by a serialization token |


`docs/DEMOS.md` walks through every scenario.

## Results

Measured outputs from this repo's own programs; timings vary by machine.

`demos/run_wordcount.sh` builds one word-count program two ways and times both
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
| `demos/` | the showcase (`docs/DEMOS.md`) |
| `tests/` | the test suite (`./tests/run.sh`) |


## Caveats

There are a few caveats and drawbacks that are yet to be addressed. We hope to flesh this out in the future.

- On page cache resident data the latency gains are very less. We hope that in these cases, the ergonomics makes up for the lack of speedup.
- FilC overhead: FilC is a relatively young compiler and itself reports 1.5x slowdown over optimized GCC compiled code.
- A Better way for dependency tracking using provenance tags.
	Currently, If there is a hidden dependency between two async calls, the programmer must explicitely annotate it using tags.
	```C
	fasync_tracker* tag = fasync_tracker_new(); // tag allocation

	async_call_1(tag);
	async_call_2(tag); 
	// async_call_2 will wait for async_call_1 to finish before executing
	```
