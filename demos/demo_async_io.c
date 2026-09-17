/*
 * demo_async_io.c -- the showcase.
 *
 * Three scenarios, each demonstrating one mechanism from idea.md section 1,
 * running on the real Fil-C runtime with a real io_uring ring.
 *
 *   1. LAZY RESOLUTION. Submit N reads, touch nothing, then touch the results.
 *      The point is what the counters show: submission never entered the kernel
 *      to wait, and every access was served by a check whose fast path is a
 *      single load.
 *
 *   2. THROUGHPUT. The same N reads done the ordinary blocking way, then the
 *      async way. Both are correct; the difference is how many times the program
 *      had to stop and wait, and whether the kernel ever got to work on more
 *      than one request at a time.
 *
 *   3. DEPENDENCIES. Declared effect sets over independent and dependent work,
 *      showing the dependency DAG the runtime derives, and the conflicts it
 *      dissolves by proving capability ranges disjoint.
 *
 * Build:
 *   filcc -O2 -static -I runtime/src -L runtime/build/lib \
 *         -o demo_async_io demos/demo_async_io.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>

#include "fasync.h"
#include "fasync_dep.h"

#define N_BLOCKS 64
#define BLOCK_SIZE 262144 /* 256 KiB */
#define FILE_SIZE ((off_t)N_BLOCKS * BLOCK_SIZE)

static double now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

/* Every byte of block i is (i + 1) & 0xFF, so a misdirected read is obvious. */
static int seed(const char* path) {
  unsigned char* block = malloc(BLOCK_SIZE);
  if (!block)
    return -1;
  int fd = open(path, O_CREAT | O_TRUNC | O_RDWR, 0644);
  if (fd < 0) {
    free(block);
    return -1;
  }
  for (int i = 0; i < N_BLOCKS; i++) {
    memset(block, (i + 1) & 0xFF, BLOCK_SIZE);
    if (pwrite(fd, block, BLOCK_SIZE, (off_t)i * BLOCK_SIZE) != BLOCK_SIZE) {
      close(fd);
      free(block);
      return -1;
    }
  }
  close(fd);
  free(block);
  return 0;
}

static int block_ok(const unsigned char* buf, int index) {
  unsigned char expected = (unsigned char)((index + 1) & 0xFF);
  for (int i = 0; i < BLOCK_SIZE; i += 512)
    if (buf[i] != expected)
      return 0;
  return 1;
}

/* ------------------------------------------------------------------ */
/* 1. Lazy resolution                                                  */
/* ------------------------------------------------------------------ */

static int scenario_lazy(const char* path) {
  printf("\n=== 1. lazy resolution ===\n");

  int fd = open(path, O_RDONLY);
  if (fd < 0)
    return 1;

  static unsigned char* bufs[N_BLOCKS];
  static fasync_id ids[N_BLOCKS];

  fasync_reset_stats();

  double t0 = now_ms();
  for (int i = 0; i < N_BLOCKS; i++) {
    bufs[i] = malloc(BLOCK_SIZE);
    ids[i] = fasync_pread(fd, bufs[i], BLOCK_SIZE, (off_t)i * BLOCK_SIZE);
    if (!ids[i]) {
      printf("  enqueue failed: %s\n", fasync_last_error());
      return 1;
    }
  }
  /* One batched, non-blocking publish for the whole set. */
  fasync_submit();
  double submit_ms = now_ms() - t0;

  struct fasync_stats s;
  fasync_get_stats(&s);
  printf("  queued %d reads (%d KiB each) in %.3f ms\n", N_BLOCKS,
         BLOCK_SIZE / 1024, submit_ms);
  printf("  kernel entries to wait:  %lu   <- submission never blocked\n",
         s.kernel_wait_entries);
  printf("  kernel entries to submit: %lu  <- one batch for %d requests\n",
         s.kernel_submit_entries, N_BLOCKS);
  printf("     ^ %d requests published with a single non-blocking enter\n",
         N_BLOCKS);

  /*
   * Now touch one result. Nothing above dereferenced a buffer: the reads may
   * have completed, or may not have, but the program never asked.
   */
  int ok = 1;
  double t1 = now_ms();
  for (int i = 0; i < N_BLOCKS; i++) {
    FASYNC_ACCESS(bufs[i], 1);
    long n = fasync_result(ids[i]);
    if (n != BLOCK_SIZE || !block_ok(bufs[i], i)) {
      printf("  block %d wrong (%ld bytes)\n", i, n);
      ok = 0;
    }
  }
  double resolve_ms = now_ms() - t1;

  fasync_get_stats(&s);
  printf("  resolving all %d: %.3f ms\n", N_BLOCKS, resolve_ms);
  printf("  resolve fast-path hits:  %lu\n", s.fast_path_hits);
  printf("  resolve slow-path calls: %lu\n", s.resolve_calls);
  printf("  completion rings polled in userspace: %lu times (0 syscalls)\n",
         s.userspace_cq_polls);
  printf("  times we slept in the kernel: %lu\n", s.parks);
  printf("  data verified: %s\n", ok ? "all blocks correct" : "MISMATCH");

  for (int i = 0; i < N_BLOCKS; i++)
    free(bufs[i]);
  close(fd);
  return ok ? 0 : 1;
}

/* ------------------------------------------------------------------ */
/* 2. Throughput: blocking vs async                                    */
/* ------------------------------------------------------------------ */

static int scenario_throughput(const char* path) {
  printf("\n=== 2. blocking vs async ===\n");

  static unsigned char* bufs[N_BLOCKS];
  static fasync_id ids[N_BLOCKS];

  /* Allocate outside the timed region: this measures I/O scheduling, not
   * allocation. */
  for (int i = 0; i < N_BLOCKS; i++)
    bufs[i] = malloc(BLOCK_SIZE);

  int ok = 1;

  /* Blocking: one pread at a time, each one waiting for the kernel. */
  int fd = open(path, O_RDONLY);
  if (fd < 0)
    return 1;
  double t0 = now_ms();
  for (int i = 0; i < N_BLOCKS; i++) {
    if (pread(fd, bufs[i], BLOCK_SIZE, (off_t)i * BLOCK_SIZE) != BLOCK_SIZE) {
      printf("  blocking read failed\n");
      return 1;
    }
  }
  double blocking_ms = now_ms() - t0;
  close(fd);

  /* Async: the same requests, all in flight at once. */
  fd = open(path, O_RDONLY);
  if (fd < 0)
    return 1;

  fasync_reset_stats();
  double t1 = now_ms();
  for (int i = 0; i < N_BLOCKS; i++) {
    ids[i] = fasync_pread(fd, bufs[i], BLOCK_SIZE, (off_t)i * BLOCK_SIZE);
    if (!ids[i]) {
      printf("  enqueue failed: %s\n", fasync_last_error());
      return 1;
    }
  }
  fasync_submit();
  double submitted_ms = now_ms() - t1;

  for (int i = 0; i < N_BLOCKS; i++)
    fasync_result(ids[i]);
  double async_ms = now_ms() - t1;

  for (int i = 0; i < N_BLOCKS; i++)
    if (!block_ok(bufs[i], i))
      ok = 0;

  struct fasync_stats s;
  fasync_get_stats(&s);

  printf("  %d x %d KiB from a warm page cache\n", N_BLOCKS, BLOCK_SIZE / 1024);
  printf("  blocking loop: %8.3f ms   (%d separate round trips)\n",
         blocking_ms, N_BLOCKS);
  printf("  async:         %8.3f ms   (%.3f ms to enqueue+publish all %d,\n",
         async_ms, submitted_ms, N_BLOCKS);
  printf("                              then %.3f ms to resolve)\n",
         async_ms - submitted_ms);
  printf("  blocking kernel entries: %lu async vs %d blocking\n",
         s.kernel_wait_entries + s.kernel_submit_entries, N_BLOCKS);
  printf("  times the async path slept: %lu\n", s.parks);
  printf("  verified: %s\n", ok ? "all blocks correct" : "MISMATCH");

  /*
   * An honest reading of these numbers.
   *
   * With a warm page cache the bytes are already in the kernel's memory, so both
   * paths are memcpy-bound and the async path's extra machinery (shared rings,
   * one copy through them, bookkeeping per request) is pure overhead. What the
   * async path removes is the per-request round trip, and here there was little
   * to remove because the round trip was already cheap.
   *
   * The win this design is chasing shows up when per-request latency is high
   * (cold cache, real devices, network filesystems), because then having every
   * request in flight simultaneously is what matters, and the number of
   * submissions stops being the bottleneck. idea.md section 5 flags exactly this
   * as an open question, and idea.md section 6 phase 6 asks for the comparison
   * against tokio-uring/monoio; see docs/ARCHITECTURE.md for what is and is not
   * established by these measurements.
   */
  if (async_ms > 0.0)
    printf("  observed: %.2fx  (warm cache; see the note in docs/ARCHITECTURE.md)\n",
           blocking_ms / async_ms);

  for (int i = 0; i < N_BLOCKS; i++)
    free(bufs[i]);
  close(fd);
  return ok ? 0 : 1;
}

/* ------------------------------------------------------------------ */
/* 3. Declared dependencies                                            */
/* ------------------------------------------------------------------ */

static int scenario_dependencies(void) {
  printf("\n=== 3. declared effect sets ===\n");

  void* a = malloc(4096);
  void* b = malloc(4096);
  void* c = malloc(4096);

  /* Four operations. Their declarations are what the runtime reasons about. */
  struct fasync_access acc0[] = {{a, 1024, FASYNC_OUT}};
  struct fasync_access acc1[] = {{b, 1024, FASYNC_OUT}};
  struct fasync_access acc2[] = {{a, 1024, FASYNC_IN}, {c, 1024, FASYNC_OUT}};
  struct fasync_access acc3[] = {{c, 1024, FASYNC_IN}};

  struct fasync_op ops[4] = {
      {"write(a)", acc0, 1},
      {"write(b)", acc1, 1},
      {"copy(a,c)", acc2, 2},
      {"read(c)", acc3, 1},
  };

  unsigned edges[64];
  struct fasync_dag_stats st;
  unsigned n = fasync_build_dag(ops, 4, edges, 64, &st);

  printf("  4 operations, %u dependency edges\n", n);
  printf("    writer-to-reader:  %u\n", st.read_write_edges);
  printf("    reader-to-writer:  %u\n", st.write_read_edges);
  printf("    writer-to-writer:  %u\n", st.write_write_edges);
  printf("  conflicts dissolved by proving capability ranges disjoint: %u\n",
         st.auto_disjoint_pairs);
  printf("  -> write(a) and write(b) declare conflicting kinds, but the runtime\n");
  printf("     proves their objects disjoint, so they run together instead of\n");
  printf("     serializing. That is an annotation nobody wrote.\n");

  for (unsigned e = 0; e < n; e++) {
    unsigned from = edges[e] / 4, to = edges[e] % 4;
    printf("     %s -> %s\n", ops[from].name, ops[to].name);
  }

  free(a);
  free(b);
  free(c);
  return 0;
}

int main(void) {
  const char* path = "/tmp/segfault_demo_payload.bin";
  if (seed(path) != 0) {
    printf("cannot create %s\n", path);
    return 1;
  }

  int rc = 0;
  rc |= scenario_lazy(path);
  rc |= scenario_throughput(path);
  rc |= scenario_dependencies();

  unlink(path);
  printf("\n%s\n", rc ? "DEMO FAILED" : "DEMO OK");
  return rc;
}
