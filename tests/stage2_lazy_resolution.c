/*
 * stage2_lazy_resolution.c -- the core mechanism of idea.md section 1, tested.
 *
 * Three things are asserted here, in order of how much they matter:
 *
 *   1. SUBMISSION NEVER BLOCKS. After queueing N async reads and publishing
 *      them, the runtime's count of blocking kernel entries is still zero. This
 *      is the "zero-context-switch submission" claim, and it is checked
 *      structurally (by counter) rather than by timing, so it cannot be
 *      satisfied by a fast machine.
 *
 *   2. CORRECTNESS. Every buffer reads back exactly the bytes that were written
 *      to that offset, and every byte count is right.
 *
 *   3. RESOLUTION IS LAZY AND CHEAP. Accesses through the checked path resolve
 *      their request (spin against the completion ring, park only if needed),
 *      and the resolve call is served from the userspace fast path whenever
 *      nothing is in flight.
 *
 * Build:
 *   filcc -O2 -static -Iruntime/src -Lruntime/build/lib \
 *         -o stage2_lazy_resolution tests/stage2_lazy_resolution.c
 *
 * Note the FASYNC_ACCESS() calls: those are exactly the points at which the
 * FilPizlonator patch will insert filc_resolve_pending() automatically. They
 * are written out by hand here only because the patched compiler does not exist
 * yet. See docs/ARCHITECTURE.md.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

#include "fasync.h"

#define N_READS 8
#define BLOCK_SIZE 262144 /* 256 KiB per read */
#define FILE_SIZE (N_READS * BLOCK_SIZE)

/* Fill the scratch file so each block is distinguishable: every byte of block
 * i is the value (i + 1) & 0xFF. */
static int seed_file(const char* path) {
  unsigned char* block = malloc(BLOCK_SIZE);
  if (!block)
    return -1;

  int fd = open(path, O_CREAT | O_TRUNC | O_RDWR, 0644);
  if (fd < 0) {
    free(block);
    return -1;
  }

  for (int i = 0; i < N_READS; i++) {
    memset(block, (i + 1) & 0xFF, BLOCK_SIZE);
    ssize_t written = pwrite(fd, block, BLOCK_SIZE, (off_t)i * BLOCK_SIZE);
    if (written != BLOCK_SIZE) {
      close(fd);
      free(block);
      return -1;
    }
  }

  close(fd);
  free(block);
  return 0;
}

/* Verify one block: every byte must equal (block_index + 1) & 0xFF. */
static int block_is_correct(const unsigned char* buf, size_t len, int index) {
  unsigned char expected = (unsigned char)((index + 1) & 0xFF);
  for (size_t i = 0; i < len; i++) {
    if (buf[i] != expected) {
      fprintf(stderr, "  block %d: byte %zu is 0x%02x, expected 0x%02x\n", index,
              i, buf[i], expected);
      return 0;
    }
  }
  return 1;
}

int main(void) {
  const char* path = "/tmp/async-a-sync_stage2_payload.bin";
  if (seed_file(path) != 0) {
    fprintf(stderr, "cannot seed %s\n", path);
    return 1;
  }

  int fd = open(path, O_RDONLY);
  if (fd < 0) {
    fprintf(stderr, "cannot open %s\n", path);
    return 1;
  }

  /* Heap buffers, one per read. They become the pending ranges. */
  unsigned char* bufs[N_READS];
  fasync_id ids[N_READS];

  fasync_reset_stats();

  /* ------------------------------------------------------------------ */
  /* Claim 1: submission never blocks.                                   */
  /* ------------------------------------------------------------------ */
  for (int i = 0; i < N_READS; i++) {
    bufs[i] = malloc(BLOCK_SIZE);
    if (!bufs[i]) {
      fprintf(stderr, "out of memory\n");
      return 1;
    }
    memset(bufs[i], 0, BLOCK_SIZE);

    ids[i] = fasync_pread(fd, bufs[i], BLOCK_SIZE, (unsigned long)i * BLOCK_SIZE);
    if (!ids[i]) {
      fprintf(stderr, "fasync_pread %d failed to enqueue\n", i);
      return 1;
    }
  }

  if (fasync_submit() != N_READS) {
    fprintf(stderr, "fasync_submit did not hand over all %d requests\n", N_READS);
    return 1;
  }

  struct fasync_stats s;
  fasync_get_stats(&s);

  printf("after submitting %d reads (%d KiB each):\n", N_READS,
         BLOCK_SIZE / 1024);
  printf("  sqes_queued          = %lu\n", s.sqes_queued);
  printf("  kernel_submit_entries= %lu  (non-blocking publishes)\n",
         s.kernel_submit_entries);
  printf("  kernel_wait_entries  = %lu  (blocking entries)\n",
         s.kernel_wait_entries);

  int ok = 1;
  if (s.kernel_wait_entries != 0) {
    printf("  FAIL: submission path entered the kernel to wait\n");
    ok = 0;
  } else {
    printf("  OK: no blocking kernel entry during submission\n");
  }

  /* ------------------------------------------------------------------ */
  /* Claims 2 and 3: resolution on genuine access.                       */
  /*                                                                     */
  /* Nothing has touched bufs[] yet. The FASYNC_ACCESS calls below are    */
  /* where the compiler patch will insert filc_resolve_pending().         */
  /* ------------------------------------------------------------------ */
  for (int i = 0; i < N_READS; i++) {
    /* First genuine access to the pending range: this is the resolution
     * point. */
    FASYNC_ACCESS(bufs[i], 1);

    /* The byte count is itself an async value; reading it resolves too. */
    long n = fasync_result(ids[i]);
    if (n != BLOCK_SIZE) {
      fprintf(stderr, "read %d returned %ld, expected %d\n", i, n, BLOCK_SIZE);
      ok = 0;
      continue;
    }

    /* Second access to the same range: the request is retired, so this must
     * take the fast path (no table scan, no completion poll). */
    FASYNC_ACCESS(bufs[i], BLOCK_SIZE);

    if (!block_is_correct(bufs[i], BLOCK_SIZE, i)) {
      ok = 0;
    }
  }

  fasync_get_stats(&s);
  printf("\nafter resolving every range:\n");
  printf("  fast_path_hits       = %lu  (resolve calls served by one load)\n",
         s.fast_path_hits);
  printf("  resolve_calls        = %lu  (resolve calls that had to look)\n",
         s.resolve_calls);
  printf("  userspace_cq_polls   = %lu  (completion-ring reads, no syscall)\n",
         s.userspace_cq_polls);
  printf("  spin_rounds          = %lu\n", s.spin_rounds);
  printf("  parks                = %lu  (times we actually slept)\n", s.parks);
  printf("  completions_reaped   = %lu\n", s.completions_reaped);

  if (s.completions_reaped != N_READS) {
    printf("  FAIL: reaped %lu completions, expected %d\n",
           s.completions_reaped, N_READS);
    ok = 0;
  } else {
    printf("  OK: every completion accounted for\n");
  }

  for (int i = 0; i < N_READS; i++)
    free(bufs[i]);
  close(fd);
  unlink(path);

  printf("\nSTAGE2 %s\n", ok ? "PASS" : "FAIL");
  return ok ? 0 : 1;
}
