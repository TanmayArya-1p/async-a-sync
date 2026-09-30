/* stage7_throughput.c -- the workload io_uring pays in: many small reads, where
 * a read is nearly all syscall cost. Both paths do identical work; what is
 * counted is kernel entries, from the runtime's own counters. Wall clock is
 * reported alongside and expected to vary. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>

#include "fasync.h"

#define READ_SIZE 64

#ifndef N_READS
#define N_READS 20000
#endif

/* The queue depth the batched path fills and drains. */
#define WAVE 200

static int failures = 0;

static void check(const char* what, int ok) {
  printf("  %-58s %s\n", what, ok ? "ok" : "FAIL");
  fflush(stdout);
  if (!ok)
    failures++;
}

static double now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

int main(void) {
  const char* path = "/tmp/async-a-sync_stage7_payload.bin";

  printf("many small reads: %d x %d bytes from a warm page cache\n\n", N_READS,
         READ_SIZE);

  /* A file of distinguishable bytes, so a read landing in the wrong place shows. */
  size_t file_size = (size_t)N_READS * READ_SIZE;
  unsigned char* seed = malloc(file_size);
  for (size_t i = 0; i < file_size; i++)
    seed[i] = (unsigned char)(i * 31 + 7);
  int fd = open(path, O_CREAT | O_TRUNC | O_RDWR, 0644);
  if (fd < 0 || pwrite(fd, seed, file_size, 0) != (ssize_t)file_size) {
    printf("cannot seed %s\n", path);
    return 1;
  }
  close(fd);
  free(seed);

  unsigned char* expected = malloc(file_size);
  fd = open(path, O_RDONLY);
  if (fd < 0)
    return 1;
  if (pread(fd, expected, file_size, 0) != (ssize_t)file_size) {
    printf("cannot read back\n");
    return 1;
  }
  lseek(fd, 0, SEEK_SET);

  unsigned char* buf = malloc(READ_SIZE);
  int ok;

  /* Blocking: one syscall per read. */
  memset(buf, 0, READ_SIZE);
  double t0 = now_ms();
  for (int i = 0; i < N_READS; i++) {
    ssize_t n = pread(fd, buf, READ_SIZE, (off_t)i * READ_SIZE);
    if (n != READ_SIZE) {
      printf("blocking read %d short\n", i);
      return 1;
    }
  }
  double blocking_ms = now_ms() - t0;

  /* Spot-check the blocking path read the right bytes. */
  ok = 1;
  for (int i = 0; i < N_READS; i += 997) {
    pread(fd, buf, READ_SIZE, (off_t)i * READ_SIZE);
    if (memcmp(buf, expected + (size_t)i * READ_SIZE, READ_SIZE) != 0)
      ok = 0;
  }
  check("blocking reads return the right bytes", ok);

  /* Async: the runtime caps requests in flight, so this works in waves -- fill
 * the queue, drain it, refill: one kernel entry per wave instead of per read. */
  fasync_reset_stats();

  double t1 = now_ms();
  int enqueued = 0;
  int async_ok = 1;
  unsigned char* wave_buf = malloc(READ_SIZE);

  while (enqueued < N_READS) {
    int wave = N_READS - enqueued;
    if (wave > WAVE)
      wave = WAVE;

    fasync_id ids[WAVE];
    for (int i = 0; i < wave; i++) {
      memset(wave_buf, 0, READ_SIZE);
      ids[i] = fasync_pread(fd, wave_buf, READ_SIZE,
                            (off_t)(enqueued + i) * READ_SIZE);
      if (!ids[i]) {
        printf("enqueue failed at %d: %s\n", enqueued + i, fasync_last_error());
        return 1;
      }
    }

    if (fasync_wait_all() != 0) {
      printf("fasync_wait_all did not drain\n");
      return 1;
    }

    /* Everything completed, so each result is a table lookup, not a wait. */
    for (int i = 0; i < wave; i++) {
      if (fasync_result(ids[i]) != READ_SIZE)
        async_ok = 0;
    }
    enqueued += wave;
  }
  double async_ms = now_ms() - t1;

  struct fasync_stats s;
  fasync_get_stats(&s);

  check("every async read completed with a full result", async_ok);

  /* Correctness of the bytes, on a small group we can inspect per-read. */
  fasync_reset_stats();
  {
    fasync_id ids[64];
    unsigned char* bufs[64];
    int bytes_ok = 1;
    for (int i = 0; i < 64; i++) {
      bufs[i] = malloc(READ_SIZE);
      memset(bufs[i], 0, READ_SIZE);
      ids[i] = fasync_pread(fd, bufs[i], READ_SIZE,
                            (off_t)(i * 137) * READ_SIZE);
    }
    if (fasync_wait_all() != 0)
      bytes_ok = 0;
    for (int i = 0; i < 64; i++) {
      if (fasync_result(ids[i]) != READ_SIZE)
        bytes_ok = 0;
      if (memcmp(bufs[i], expected + (size_t)(i * 137) * READ_SIZE,
                 READ_SIZE) != 0)
        bytes_ok = 0;
      free(bufs[i]);
    }
    check("async reads return the right bytes", bytes_ok);
  }
  free(wave_buf);

  /* Report. */
  printf("\n");
  printf("  blocking: %8.2f ms for %d reads  (%.2f us/read, %d syscalls)\n",
         blocking_ms, N_READS, blocking_ms * 1000.0 / N_READS, N_READS);
  printf("  async:    %8.2f ms for %d reads  (%.2f us/read)\n", async_ms, N_READS,
         async_ms * 1000.0 / N_READS);
  printf("\n");
  printf("  kernel entries, blocking: %d  (one pread each)\n", N_READS);
  printf("  kernel entries, async:    %lu  (submits %lu + waits %lu)\n",
         s.kernel_submit_entries + s.kernel_wait_entries,
         s.kernel_submit_entries, s.kernel_wait_entries);
  if (s.kernel_submit_entries + s.kernel_wait_entries > 0) {
    printf("  entries saved: %.1fx fewer\n",
           (double)N_READS /
               (double)(s.kernel_submit_entries + s.kernel_wait_entries));
  } else {
    printf("  entries saved: all of them\n");
  }
  printf("  completion ring polled in userspace: %lu times (0 syscalls)\n",
         s.userspace_cq_polls);
  printf("  SQEs published per submit: %.1f\n",
         s.sqes_queued ? (double)s.sqes_queued / (double)s.kernel_submit_entries
                       : 0.0);

  double ratio = async_ms > 0 ? blocking_ms / async_ms : 0;
  printf("\n  observed: %.2fx\n", ratio);

  check("async did fewer kernel entries than one-per-read",
        s.kernel_submit_entries + s.kernel_wait_entries < (unsigned long)N_READS);

  free(buf);
  free(expected);
  close(fd);
  unlink(path);

  printf("\nSTAGE7 %s\n", failures ? "FAIL" : "PASS");
  return failures ? 1 : 0;
}
