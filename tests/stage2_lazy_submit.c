/*
 * stage2_lazy_submit.c -- submission is implicit: never call fasync_submit().
 *
 * This is stage2 with the publish step removed: the workload is plain enqueue
 * calls, exactly as a program would write them, and no submit appears after.
 * Two things are asserted: (1) after enqueueing, nothing has reached the kernel
 * -- writing SQEs is a memory operation; (2) the first genuine access to a
 * pending buffer publishes the whole queue in one non-blocking enter, because a
 * resolver cannot spin for a completion that was never submitted.
 *
 * The FASYNC_ACCESS() calls stand in for the points where the patched
 * FilPizlonator will insert filc_resolve_pending() automatically; see
 * docs/ARCHITECTURE.md.
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
  const char* path = "/tmp/async-a-sync_stage2lazy_payload.bin";
  if (seed_file(path) != 0) {
    fprintf(stderr, "cannot seed %s\n", path);
    return 1;
  }

  int fd = open(path, O_RDONLY);
  if (fd < 0) {
    fprintf(stderr, "cannot open %s\n", path);
    return 1;
  }

  unsigned char* bufs[N_READS];
  fasync_id ids[N_READS];

  fasync_reset_stats();

  /* ------------------------------------------------------------------ */
  /* Phase 1: enqueue only. No submit, no access.                        */
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

  struct fasync_stats s;
  fasync_get_stats(&s);

  printf("enqueued %d reads (%d KiB each), submitted nothing:\n", N_READS,
         BLOCK_SIZE / 1024);
  printf("  sqes_queued          = %lu\n", s.sqes_queued);
  printf("  kernel_submit_entries= %lu  (must be 0: enqueue is a memory op)\n",
         s.kernel_submit_entries);

  int ok = 1;
  if (s.kernel_submit_entries != 0) {
    printf("  FAIL: something reached the kernel during plain enqueue\n");
    ok = 0;
  } else {
    printf("  OK: submission is fully implicit so far\n");
  }

  /* ------------------------------------------------------------------ */
  /* Phase 2: first genuine access publishes the whole batch lazily.     */
  /* ------------------------------------------------------------------ */
  for (int i = 0; i < N_READS; i++) {
    FASYNC_ACCESS(bufs[i], 1);

    long n = fasync_result(ids[i]);
    if (n != BLOCK_SIZE) {
      fprintf(stderr, "read %d returned %ld, expected %d\n", i, n, BLOCK_SIZE);
      ok = 0;
      continue;
    }

    FASYNC_ACCESS(bufs[i], BLOCK_SIZE);

    if (!block_is_correct(bufs[i], BLOCK_SIZE, i)) {
      ok = 0;
    }
  }

  fasync_get_stats(&s);
  printf("\nafter resolving every range (no submit was ever called):\n");
  printf("  kernel_submit_entries= %lu  (must be 1: one publish, one enter)\n",
         s.kernel_submit_entries);
  printf("  kernel_wait_entries  = %lu  (blocking entries)\n",
         s.kernel_wait_entries);
  printf("  completions_reaped   = %lu\n", s.completions_reaped);

  if (s.kernel_submit_entries != 1) {
    printf("  FAIL: expected the whole batch to publish in a single enter\n");
    ok = 0;
  } else {
    printf("  OK: first access published every queued SQE at once\n");
  }

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

  printf("\nSTAGE2-LAZY %s\n", ok ? "PASS" : "FAIL");
  return ok ? 0 : 1;
}