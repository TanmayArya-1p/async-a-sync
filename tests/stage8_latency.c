/*
 * stage8_latency.c -- the regime where overlapped execution actually pays: many
 * independent reads that each cost a device round trip, so a serial program pays
 * N round trips and an overlapped one pays about one.
 *
 * Every other measurement in this suite reads cache-resident data. The page
 * cache is dropped (posix_fadvise(DONTNEED); no root) before each timed pass.
 * It only takes here -- and not over one big file -- because separate files
 * cannot be prefetched by readahead, and because the payload must sit on a real
 * backing store (tmpfs has no device to wait for). If the drop changes nothing,
 * the program says the timings mean nothing rather than quoting an unsupported
 * ratio; O_DIRECT skips that check entirely.
 *
 * THREE ARMS:
 *   blocking  a pread per file, each waiting for the device
 *   explicit  submit everything, wait handle by handle -- what a hand-written
 *             io_uring program does, on the same runtime and substrate, isolating
 *             the question the project has to answer: does being implicit cost?
 *   implicit  submit everything, then hand the buffers to ordinary code; the
 *             compiler's hook resolves them at the first byte touched
 *
 * An independent raw-liburing baseline would not isolate the question: it would
 * also lack Fil-C's per-access checking, and the difference would be substrate,
 * not ergonomics. See stage9 for the ceiling this runs into.
 *
 * Build (needs the patched compiler -- run.sh skips this when it is not built):
 *   <patched>/filcc -O2 -static -DFASYNC_COMPILER_INSERTS_CHECKS -Iruntime/src \
 *     -Lruntime/build/lib -o stage8 tests/stage8_latency.c [dir]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>

#include "fasync.h"

/*
 * Arm C is only implicit if the compiler inserts the hook. Built with the stock
 * compiler there is no resolution call anywhere in the program, so arm C reads
 * unfilled buffers and its checksum comes out short -- which is exactly the
 * failure this test caught when it was first run against the wrong toolchain.
 * Refuse to build that way.
 */
#ifndef FASYNC_COMPILER_INSERTS_CHECKS
#error "build with the patched compiler and -DFASYNC_COMPILER_INSERTS_CHECKS"
#endif

#ifndef N_FILES
#define N_FILES 512
#endif
#define FILE_BYTES 4096
#define WAVE 512 /* must be <= the runtime's request table */

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

/*
 * An ordinary function, the same one every arm uses. Nothing about it is
 * async-aware; in the implicit arm the compiler puts a resolution check in front
 * of its load.
 */
__attribute__((noinline))
static unsigned long checksum(const unsigned char* p, size_t n) {
  unsigned long sum = 0;
  for (size_t i = 0; i < n; i++)
    sum += p[i];
  return sum;
}

static char paths[N_FILES][256];

static void drop_caches(void) {
  for (int i = 0; i < N_FILES; i++) {
    int fd = open(paths[i], O_RDONLY);
    if (fd >= 0) {
      posix_fadvise(fd, 0, FILE_BYTES, POSIX_FADV_DONTNEED);
      close(fd);
    }
  }
}

int main(int argc, char** argv) {
  const char* dir = argc > 1 ? argv[1] : ".";
  int use_direct = argc > 2 && strcmp(argv[2], "direct") == 0;

  printf("latency regime: %d files of %d bytes, %s\n\n", N_FILES, FILE_BYTES,
         use_direct ? "O_DIRECT (every read goes to the device)"
                    : "page cache dropped per pass");

  unsigned char* seed = malloc(FILE_BYTES);
  memset(seed, 0x33, FILE_BYTES);

  for (int i = 0; i < N_FILES; i++) {
    snprintf(paths[i], sizeof(paths[i]), "%s/lat_%04d.bin", dir, i);
    int fd = open(paths[i], O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (fd < 0 || pwrite(fd, seed, FILE_BYTES, 0) != FILE_BYTES) {
      printf("cannot create %s (is the directory writable?)\n", paths[i]);
      return 1;
    }
    fsync(fd);
    close(fd);
  }
  free(seed);

  int fd[N_FILES];
  unsigned char* buf[N_FILES];
  unsigned long expect = (unsigned long)0x33 * FILE_BYTES;
  for (int i = 0; i < N_FILES; i++) {
    /* Aligned, because O_DIRECT requires it and it costs nothing otherwise. */
    void* p = 0;
    if (posix_memalign(&p, 4096, FILE_BYTES) != 0)
      return 1;
    buf[i] = p;
    memset(buf[i], 0, FILE_BYTES);
    fd[i] = open(paths[i], O_RDONLY | (use_direct ? O_DIRECT : 0));
    if (fd[i] < 0) {
      printf("cannot reopen %s%s\n", paths[i],
             use_direct ? " with O_DIRECT (unsupported here?)" : "");
      return 1;
    }
  }

  /* Establish that dropping the cache costs something, before claiming anything
   * about the ratio between the arms. O_DIRECT needs no such check: it does not
   * use the page cache at all. */
  double regime = 0;
  int regime_ok = use_direct;
  if (!use_direct) {
    double warm0 = now_ms();
    for (int i = 0; i < N_FILES; i++)
      if (pread(fd[i], buf[i], FILE_BYTES, 0) != FILE_BYTES)
        return 1;
    double warm = now_ms() - warm0;

    drop_caches();
    double cold0 = now_ms();
    for (int i = 0; i < N_FILES; i++)
      if (pread(fd[i], buf[i], FILE_BYTES, 0) != FILE_BYTES)
        return 1;
    double cold_probe = now_ms() - cold0;

    regime = cold_probe / (warm > 0 ? warm : 1e-9);
    regime_ok = regime > 1.5;
    printf("  warm cache:  %7.2f ms  (%5.2f us/file)\n", warm,
           warm * 1000.0 / N_FILES);
    printf("  dropped:     %7.2f ms  (%5.2f us/file)  -> %.1fx\n\n", cold_probe,
           cold_probe * 1000.0 / N_FILES, regime);
    if (!regime_ok) {
      printf("  !! dropping the cache changed nothing on this filesystem, so there is\n"
             "     no device latency here to overlap. The timings below are still\n"
             "     correct but the comparison between them means nothing.\n\n");
    }
  } else {
    printf("  no warm/cold check: O_DIRECT reads reach the device either way.\n\n");
  }

  unsigned long sum_a = 0, sum_b = 0, sum_c = 0;

  /* ------------------------------------------------------------------ */
  /* A: blocking                                                         */
  /* ------------------------------------------------------------------ */
  drop_caches();
  fasync_reset_stats();

  double t_a = now_ms();
  for (int i = 0; i < N_FILES; i++) {
    if (pread(fd[i], buf[i], FILE_BYTES, 0) != FILE_BYTES)
      return 1;
    sum_a += checksum(buf[i], FILE_BYTES);
  }
  double a_ms = now_ms() - t_a;

  /* ------------------------------------------------------------------ */
  /* B: explicit -- submit everything, then wait handle by handle        */
  /* ------------------------------------------------------------------ */
  drop_caches();
  fasync_reset_stats();

  double t_b = now_ms();
  {
    fasync_id ids[N_FILES];
    int enqueued = 0;
    while (enqueued < N_FILES) {
      int wave = N_FILES - enqueued;
      if (wave > WAVE)
        wave = WAVE;
      for (int i = 0; i < wave; i++) {
        memset(buf[enqueued + i], 0, FILE_BYTES);
        ids[i] = fasync_pread(fd[enqueued + i], buf[enqueued + i], FILE_BYTES, 0);
        if (!ids[i]) {
          printf("  enqueue failed: %s\n", fasync_last_error());
          return 1;
        }
      }
      fasync_submit();
      for (int i = 0; i < wave; i++) {
        if (fasync_result(ids[i]) != FILE_BYTES)
          return 1;
        sum_b += checksum(buf[enqueued + i], FILE_BYTES);
      }
      enqueued += wave;
    }
  }
  double b_ms = now_ms() - t_b;
  struct fasync_stats sb;
  fasync_get_stats(&sb);

  /* ------------------------------------------------------------------ */
  /* C: implicit -- submit everything, then just use the buffers         */
  /* ------------------------------------------------------------------ */
  drop_caches();
  fasync_reset_stats();

  double t_c = now_ms();
  {
    fasync_id ids[N_FILES];
    int enqueued = 0;
    while (enqueued < N_FILES) {
      int wave = N_FILES - enqueued;
      if (wave > WAVE)
        wave = WAVE;
      for (int i = 0; i < wave; i++) {
        memset(buf[enqueued + i], 0, FILE_BYTES);
        ids[i] = fasync_pread(fd[enqueued + i], buf[enqueued + i], FILE_BYTES, 0);
        if (!ids[i]) {
          printf("  enqueue failed: %s\n", fasync_last_error());
          return 1;
        }
      }
      fasync_submit();

      /* No wait anywhere: the compiler resolves each buffer at the first byte
       * checksum() touches. */
      for (int i = 0; i < wave; i++)
        sum_c += checksum(buf[enqueued + i], FILE_BYTES);

      /* Bookkeeping only (releases slots for the next wave). An earlier draft
       * checked the buffers here -- useless, since the check is itself an
       * instrumented access that resolves whatever it looks at. The checksums
       * above are the real check. */
      for (int i = 0; i < wave; i++)
        fasync_result(ids[i]);
      enqueued += wave;
    }
  }
  double c_ms = now_ms() - t_c;
  struct fasync_stats sc;
  fasync_get_stats(&sc);

  /* ------------------------------------------------------------------ */
  /* Report                                                              */
  /* ------------------------------------------------------------------ */
  printf("  %-26s %9s %9s\n", "arm", "ms", "us/file");
  printf("  %-26s %9.2f %9.2f\n", "A blocking", a_ms, a_ms * 1000.0 / N_FILES);
  printf("  %-26s %9.2f %9.2f\n", "B explicit (wait per handle)", b_ms,
         b_ms * 1000.0 / N_FILES);
  printf("  %-26s %9.2f %9.2f\n", "C implicit (no wait written)", c_ms,
         c_ms * 1000.0 / N_FILES);

  printf("\n");
  printf("  C vs A (overlap, the ergonomic win):     %6.2fx\n",
         a_ms / (c_ms > 0 ? c_ms : 1e-9));
  printf("  C vs B (does being implicit cost?):      %6.2fx\n",
         b_ms / (c_ms > 0 ? c_ms : 1e-9));
  printf("  kernel submits, B: %lu   C: %lu   (for %d files)\n",
         sb.kernel_submit_entries, sc.kernel_submit_entries, N_FILES);
  printf("  kernel waits,   B: %lu   C: %lu\n", sb.kernel_wait_entries,
         sc.kernel_wait_entries);
  printf("  accesses answered by the range memo, C: %lu\n", sc.memo_hits);

  check("all three arms read the same bytes", sum_a == sum_b && sum_b == sum_c);
  check("arm A read the whole payload", sum_a == expect * N_FILES);
  check("the compiler's hook resolved arm C's buffers", sc.resolve_calls > 0);
  check("the implicit arm used far fewer kernel entries than one per read",
        sc.kernel_submit_entries + sc.kernel_wait_entries < (unsigned long)N_FILES);
  check("the implicit arm never waited at submission", sc.kernel_wait_entries == 0);
  if (regime_ok) {
    check("overlap beat serial execution on an uncached device", c_ms < a_ms);
  } else {
    printf("  %-58s %s\n", "overlap beat serial (SKIPPED: no device latency here)",
           "-");
  }

  for (int i = 0; i < N_FILES; i++) {
    close(fd[i]);
    free(buf[i]);
    unlink(paths[i]);
  }

  printf("\nSTAGE8 %s\n", failures ? "FAIL" : "PASS");
  return failures ? 1 : 0;
}
