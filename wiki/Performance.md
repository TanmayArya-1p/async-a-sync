# Demos and performance

Every number below comes from this repository's own programs, on Linux
x86-64 with an NVMe SSD. Results depend on the storage device and on other
I/O on the machine. Each timing demo first checks that dropping the page
cache makes reads slower, and warns if it does not, because then there is no
device latency to overlap.

## Demos

Run `make help` for the full list.

### Annotated calls (`make demo-pragma`, or `make demo-pragma-<name> ARGS=...`)

Each demo prints a table, a `=>` result line, and `DEMO OK` when its checks
pass. A demo's source holds only the code it demonstrates. Setup, timing and
checks are in `demos/pragma/pragma_report.hh`.

| Demo | Shows |
|---|---|
| `hello` | one annotated `pread`: the call marks the buffer and queues a request, and the first read of the buffer sends it |
| `lifecycle` | `openat`, `pwrite`, `fsync`, `pread`, `close` issued back to back, kept in order by `w_dep`/`r_dep` |
| `ordering` | conflicting calls on one fd wait for each other; independent calls batch into one kernel submit |
| `coldread` | the same loop calling `pread` and an annotated `async_pread` over 512 cold files, next to hand-written `fasync_*` |
| `scaling` | that comparison for 1 to 2048 files, with the time spent inside each annotated call |
| `overlap` | reading and hashing 256 files, with the reads and the hashing also timed alone |

### A runtime of the program's own (`make demo-rpc`)

Both demos send calls to a loopback TCP server through `runtime=rpc`, a
runtime in the demos' own source. See
[The RPC runtime example](RPC-Runtime.md).

| Demo | Shows |
|---|---|
| `counter` | `step` and `get` calls on `runtime=rpc` alone. Each reply lands in a `bout=` value, and reading the value is the only wait. Two `get` calls are in flight together and only the `step` and the `get` after it wait for a lock: four 50 ms calls finish in about 150 ms instead of 200 |
| `upload` | two runtimes in one loop: each file is read with io_uring and its buffer uploaded over rpc. Each upload waits for its own read, the reads still reach the kernel in one submit, and the server's checksums match the files |

### Explicit API (`make all-demos`)

| Demo | Shows |
|---|---|
| `demo-plain` | request every file, count every file, with no submit or wait anywhere |
| `demo-wordcount` | one word-count program built three ways (GCC, Fil-C blocking, Fil-C implicit) over 512 files |
| `demo-provenance` | a write and a read on two descriptors of one file, ordered by a provenance token |
| `demo-async` | lazy resolution, blocking versus issuing everything, the dependency DAG |

`make disasm` and `make cfg` show the code the patched compiler inserts at
an access, next to GCC's plain load.

## Results

### Annotated calls

| Demo | Result |
|---|---|
| `coldread` | about **3.5x** faster than blocking `pread`, as fast as the hand-written `fasync_*` version |
| `overlap` | about **1.8x** faster than read-then-hash |
| `scaling` | an annotated call costs 1–2 µs with up to 1024 calls in flight |

At 2048 files, the gain in `scaling` is lower and varies more between runs:
calls wait for room in the 1024-entry request table.

### Word count (`demos/wordcount/run_wordcount.sh`)

One synchronous-looking program, built blocking and implicit, over 512 files
with the page cache dropped:

```text
  sync        25.34 ms  (338154 words)
  implicit    13.80 ms  (338154 words)
  implicit finish in 1.84x the time
```

### Throughput regime (`tests/stage7_throughput.c`)

20,000 reads of 64 bytes from a warm page cache. Blocking code pays one
kernel entry per read. The implicit program pays 1302 for all 20,000: it
submits 200 per batch and polls the completion ring in userspace.

```text
  blocking:  14.98 ms  (20000 kernel entries, one per read)
  async:     13.61 ms  (1302 kernel entries: 100 submits + 1202 waits)
  entries saved: 15.4x         observed wall clock: 1.10x
```

### Latency regime (`tests/stage8_latency.c`)

512 files of 4 KiB, with the page cache dropped before each pass. Three
versions read the same bytes:

- **A** blocks per file;
- **B** uses io_uring with an explicit wait;
- **C** is the implicit program.

C's first access publishes the whole batch lazily instead of submitting
eagerly:

```text
  cache dropped:  22.37 ms  (43.68 us/file; 11x cold vs warm)
  C vs A (implicit vs blocking):            2.71x
  C vs B (implicit vs explicit io_uring):   1.22x
```

## Where the gains come from, and where they stop

- **Overlap.** Independent device round trips run concurrently instead of one
  after another. This dominates on cold reads.
- **Batching.** Deferred submission turns N kernel entries into about one.
  This dominates on cached, syscall-bound workloads, but the absolute gain is
  small.
- **Fil-C's own overhead** is 1.5–4x over GCC, so the overlap has to pay for
  it. Warm-cache workloads gain little.
- **The device is not saturated.** The implicit path reaches about 52 kIOPS,
  where plain threads sustain about 184 kIOPS. The gap comes from io_uring's
  `io-wq` worker ceiling, which has to be tuned per workload
  (`tests/stage9_device_parallelism.c`).
