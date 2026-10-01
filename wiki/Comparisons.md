# Comparisons

**Source:** [`demos/comparison/`](../demos/comparison/)
**Run:** `make demo-compare` (every comparison), `make demo-compare-io-read`,
or `demos/comparison/run_comparison.sh io_read [OUT_DIR] [files] [bytes] [passes]`

A comparison takes one task and writes it twice: once with an annotated call
on this project's runtime, and once the way the task is usually written
without it. Both versions run the same workload under the same timing, and
the script prints them side by side: how long each took and how many lines
of code the task needed.

## Running one

```sh
make demo-compare-io-read
make demo-compare-io-read ARGS="1024 65536 5"   # files, bytes per file, passes
```

The baseline needs liburing's headers: `apt install liburing-dev`. To build
against another copy, set `LIBURING_CFLAGS` (for example `-I<prefix>/include`)
and `LIBURING_LIBS` (for example `<prefix>/lib/liburing.a`).

## How a comparison stays fair

Each comparison is a pair of programs, `<task>_filc_async.c` and
`<task>_<baseline>.c`, on the shared harness in `compare_utils.h`:

- **Same workload.** The harness writes the same files for both: the same
  count, size and bytes.
- **Same timed region.** Before each pass, the harness empties the buffers
  and drops the files from the page cache. It then times one call of the
  task function and checks its result against the files' checksum.
- **Same warm-up.** One untimed pass runs first, so neither side's timing
  includes setting up its ring.
- **No special modes.** Neither side uses SQPOLL, O_DIRECT, registered files
  or registered buffers, since the runtime uses none of them. Neither queues
  more than 1024 requests, the size of the runtime's request table.
- **Separate compilers.** Our version is built by the patched Fil-C compiler.
  The baseline is built by the host `cc -O2`, because a Fil-C program cannot
  map io_uring's rings itself. That is why the runtime has a native half. So
  the harness also times the checksum alone, over bytes already in memory,
  to show the compute part.
- **Lines counted the same way.** The script counts the lines between each
  file's `/* task */` and `/* end task */` markers, without blank or
  comment lines. Everything the task needs is between them, including the
  baseline's ring setup.
- **A broken version fails.** The task returns the sum of the files'
  checksums, and the harness checks it every pass. A version that reads a
  buffer before its read lands gets the wrong sum and fails.

## io_read: read cold files, then checksum them

The task reads every file into its own buffer, then returns the sum of the
buffers' FNV-1a checksums.

**With an annotated call** (`io_read_filc_async.c`, 12 lines):

```c
FILC_ASYNC(io_uring, FILC_OP(pread), FILC_BOUT(buf))
void* async_pread(int fd, void* buf, size_t len, unsigned long offset) {
  return 0;
}

static uint64_t read_all(struct files* f) {
  for (int i = 0; i < f->n; i++)
    async_pread(f->fd[i], f->buf[i], f->bytes, 0); // queues the read
  uint64_t sum = 0;
  for (int i = 0; i < f->n; i++)
    sum += fnv1a(f->buf[i], f->bytes); // waits for that read
  return sum;
}
```

**With liburing** (`io_read_liburing.c`, 29 lines): the code
- opens a ring;
- takes a submission entry per file, preps it with `io_uring_prep_read`,
  tags it with the file's index, and submits them all at once;
- reaps one completion per file, checks its result, and checksums the
  buffer the completion names.

Both versions send every read to the kernel before checksumming. The
liburing version checksums buffers in completion order; ours goes in file
order, and each checksum waits for its own read.

**Results** (Linux x86-64, NVMe SSD; median of 5 passes):

| Workload | Version | Total ms | Checksum alone, ms | MB/s | Task lines |
|---|---|---|---|---|---|
| 512 files of 4 KiB | FILC_ASYNC (Fil-C) | 6.5 | 2.6 | 307 | 12 |
| | liburing (`cc -O2`) | 5.3 | 2.6 | 375 | 29 |
| 1024 files of 64 KiB | FILC_ASYNC (Fil-C) | 116.0 | 86.1 | 552 | 12 |
| | liburing (`cc -O2`) | 113.5 | 84.2 | 564 | 29 |

- **Code.** The annotated version is less than half the code. It has no
  ring, no completion loop and no error path. A failed read leaves its
  buffer unfilled, and the checksum check catches it.
- **Time.** Ours is about 2.4 µs per file slower in both workloads. That is
  22% at 4 KiB, and 2% at 64 KiB, where the device and the checksum
  dominate.
- **The cost is per call, not per byte.** It covers the stub, the pending
  mark, and the task each call carries.
- **The compiler cost is small here.** The checksum costs about the same
  under both compilers: FNV-1a is a chain of multiplies either way.

## Adding a comparison

1. **Write `<task>_filc_async.c`** with the annotated version.
2. **Write `<task>_<baseline>.c`** with the usual way, built by the host
   compiler. Each file holds the task between the markers, and a `main`
   that calls `setup`, `run_passes` and `report` from `compare_utils.h`.
3. **Add the task to `run_comparison.sh`**, which names its baseline and
   how to build it.
4. **Add it to `COMPARISONS` in the Makefile**, then add a target like
   `demo-compare-io-read`.
5. **Add a `run_comparison <task>` line to `tests/run.sh`.**
