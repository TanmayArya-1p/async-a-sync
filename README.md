# async-a-sync

**async-a-sync** is an LLVM-based toolkit for transparent asynchronous I/O:
synchronous-looking C executes as an overlapped `io_uring` workload, with no async
syntax anywhere in the program. Instead of await-style annotations, the resolution
check is injected into `FilPizlonator`, the capability-instrumentation pass of
[Fil-C](https://github.com/pizlonator/fil-c), where it is emitted alongside the
bounds check the compiler already places on every access - no markers, no `unsafe`,
no escape hatch. The runtime adds zero-context-switch submission, provenance that
survives pointer arithmetic, lazy spin-then-park resolution, and an effect-set
dependency DAG that proves non-conflict from capability extents alone. The
completion layer is backend-agnostic by construction: any queue the resolver can
poll drives the same mechanism, so sockets, timers and RDMA share one resolution
path.

Implementation of the design in `idea.md` (§6 phase 1, plus the dependency work from
§3). What is built, what is measured, and what does not work are all in
`docs/ARCHITECTURE.md`.

## What it does

You write ordinary blocking-looking code. Underneath, the I/O is submitted to an
`io_uring` ring without ever waiting, the value the syscall produced carries a tag
naming its pending request, and the first real access through that value - or
anything derived from it - resolves it transparently.

```c
fasync_id id = fasync_pread(fd, buf, len, offset);  /* returns immediately */
...
int field = *(int*)(buf + 4096);   /* first genuine access: resolves here */
```

The only line there that is not ordinary C is the one that *submits*; every access
is a plain load, and with the patched compiler there is no marker anywhere in the
program. Without it, the same points are marked by hand with `FASYNC_ACCESS()`,
which expands to exactly the call `FilPizlonator` emits. See "The compiler half"
below.

## Results

From `demos/demo_async_io.c`, 64 × 256 KiB from a warm page cache:

```
queued 64 reads in 0.049 ms
kernel entries to wait: 0        <- submission never blocked
kernel entries to submit: 1      <- one batch for 64 requests
resolving all 64: 6.39 ms  (281832 userspace completion-ring polls, 0 parks)
```

`tests/stage7_throughput.c` pushes on the other end of the range - 20 000 reads
of 64 bytes, where the syscall is nearly all of the cost - and reaches the same
conclusion: 30x fewer kernel entries, wall clock unchanged.

`demos/demo_plain_io.c` is the ergonomic half, and the clearest evidence for the
claim in the abstract: a `count_words()` that has never heard of `io_uring`, reading
four buffers that are still in flight, with no marker anywhere in the program.

`demos/demo_wordcount.c` is both halves in one program - the same word count
written twice, once blocking and once implicit, over 512 files with the page cache
dropped. 27.4 ms against 13.3 ms, one kernel submit for 512 files and no wait
written anywhere. That is 2.1x, and it is bounded by how much parallelism the disk
offers rather than by the software: `tests/stage9_device_parallelism.c` measures
the ceiling and §8 says where the rest of it is going.

The mechanism works and the counters prove it. The honest reading of the timing is
in `docs/ARCHITECTURE.md` §8: on cache-resident data this is **not** faster, and
the reason is explained there rather than glossed over.

## Quickstart

Needs a Fil-C 0.685 distribution and a Fil-C source checkout. The defaults expect
both under `vendor/`:

```sh
# 1. the prebuilt toolchain
mkdir -p vendor && cd vendor
curl -LO https://github.com/pizlonator/fil-c/releases/download/v0.685/filc-0.685-linux-x86_64.tar.xz
tar xf filc-0.685-linux-x86_64.tar.xz && cd ..

# 2. the sources (partial clone; the full tree is not needed)
git clone --depth 1 --filter=blob:none --sparse \
  -b deluge https://github.com/pizlonator/fil-c.git vendor/fil-c-src
(cd vendor/fil-c-src && git sparse-checkout set filc libpas llvm/lib/Transforms \
  llvm/include/llvm/Transforms clang/lib/CodeGen)

# 3. build and test
./runtime/build.sh
./tests/run.sh
```

Point `FILC_ROOT` / `FILC_SRC` elsewhere if your checkout lives somewhere else.

## Layout

| Path | What it is |
|---|---|
| `runtime/` | the Fil-C runtime extension: rings, resolution, provenance, dependencies |
| `compiler/` | the modified FilPizlonator pass that automates the resolution hook |
| `tests/` | the test suite (`./tests/run.sh`) |
| `demos/` | the showcase |
| `docs/ARCHITECTURE.md` | design, findings, measurements, limitations |
| `idea.md` | the original design document |

## The compiler half

The hook that resolves a pending value needs to be inserted by the compiler, and
that compiler now exists: `compiler/upstream-overrides/FilPizlonator.cpp` is this
repo's modified copy of the pass (installed over the vendor source by
`compiler/build.sh`), which inserts the resolution call automatically. A program
compiled that way resolves async results with no explicit call at the access site
anywhere in its source -
`FASYNC_ACCESS()` compiles to nothing under `-DFASYNC_COMPILER_INSERTS_CHECKS`,
and `tests/stage4_compiler_hook.c` checks that the resolution really does happen.

Without the patched compiler, programs mark the same points with the
`FASYNC_ACCESS()` macro, which expands to exactly the call the compiler emits. The
runtime is identical either way.

Getting there turned up a genuine constraint - the emitted call has to land on
*native* runtime code, because a `pizlonated_*` entry point is a descriptor stub
rather than the function - which is why the resolution policy lives in the trusted
half. `docs/ARCHITECTURE.md` §2.5 explains it.

## Reading order

`docs/ARCHITECTURE.md` is the substantive document. Its §2 covers the four things
the survey of Fil-C established - including one that closes a risk `idea.md` §5
lists as open, and two that forced genuine design changes - and §7 lists what does
not work.
