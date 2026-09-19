/*
 * demo_wordcount.c -- the same word count, written twice.
 *
 * This is the demo the project exists to make. Both halves below are ordinary C
 * that counts words in a list of files. The only difference is how the bytes get
 * there:
 *
 *   blocking        pread() each file, then count it
 *   implicit async  submit a read for every file, then count them -- with no wait
 *                   written anywhere, because touching a buffer is what waits
 *
 * Neither version mentions async. `count_words()` is shared between them and has
 * never heard of io_uring; in the second arm the compiler's hook resolves each
 * buffer at the first byte it touches. That is the whole claim: the program is
 * written as blocking code and executed overlapped.
 *
 * The workload is many small files with the page cache dropped, which is the
 * regime where overlap is worth anything -- every file costs a device round trip,
 * a serial program pays one per file, and an overlapped one pays about one for
 * the batch. On a warm cache there is nothing to overlap and this demo would
 * honestly show no difference, which is why it drops the cache and says so.
 *
 * Pass a directory on a real filesystem as the first argument; on tmpfs there is
 * no device to wait for and the comparison is meaningless (the demo detects that
 * and tells you).
 *
 * Build (needs the patched compiler -- run.sh skips this when it is not built):
 *   <patched>/filcc -O2 -static -DFASYNC_COMPILER_INSERTS_CHECKS -Iruntime/src \
 *     -Lruntime/build/lib -o demo_wordcount demos/demo_wordcount.c [dir] [files]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>

#include "fasync.h"

/*
 * Without the compiler-inserted hook the second arm has nothing to resolve its
 * buffers, so it would count zeros and the demo would silently be a lie. Refuse
 * to build that way.
 */
#ifndef FASYNC_COMPILER_INSERTS_CHECKS
#error "build with the patched compiler and -DFASYNC_COMPILER_INSERTS_CHECKS"
#endif

#define N_FILES 512
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

/* ------------------------------------------------------------------ */
/* The ordinary code. One implementation, used by both arms.           */
/* ------------------------------------------------------------------ */

/* noinline so that the access really happens in a callee: the point is that a
 * function which has never heard of any of this can be handed a buffer that is
 * still being filled. */
__attribute__((noinline))
static size_t count_words(const char* p, size_t n) {
  size_t words = 0;
  int in_word = 0;
  for (size_t i = 0; i < n; i++) {
    int separator = (p[i] == ' ' || p[i] == '\n');
    if (!separator && !in_word)
      words++;
    in_word = !separator;
  }
  return words;
}

/* ------------------------------------------------------------------ */
/* A corpus of small files                                             */
/* ------------------------------------------------------------------ */

static const char* WORD_LIST[] = {"alpha",  "beta",  "gamma",   "delta",
                                  "epsilon", "zeta", "eta",    "theta",
                                  "iota",   "kappa", "lambda", "mu"};
#define NWORDS ((int)(sizeof(WORD_LIST) / sizeof(WORD_LIST[0])))

static unsigned int g_rand = 20260918;

static unsigned int next_rand(void) {
  g_rand = g_rand * 1103515245u + 12345u;
  return g_rand >> 16;
}

static char paths[N_FILES][256];

static void write_corpus(const char* dir) {
  unsigned char* b = malloc(FILE_BYTES);
  for (int i = 0; i < N_FILES; i++) {
    snprintf(paths[i], sizeof(paths[i]), "%s/wordcount_%04d.txt", dir, i);

    size_t n = 0;
    while (n < FILE_BYTES) {
      const char* w = WORD_LIST[next_rand() % NWORDS];
      size_t wl = strlen(w);
      if (n + wl + 1 > FILE_BYTES)
        break;
      memcpy(b + n, w, wl);
      n += wl;
      b[n++] = (next_rand() & 8) ? '\n' : ' ';
    }
    memset(b + n, ' ', FILE_BYTES - n);

    int fd = open(paths[i], O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (fd < 0 || pwrite(fd, b, FILE_BYTES, 0) != FILE_BYTES) {
      printf("cannot write %s\n", paths[i]);
      exit(1);
    }
    fsync(fd); /* so the data is on the device, not just in the writer's cache */
    close(fd);
  }
  free(b);
}

static void drop_caches(void) {
  for (int i = 0; i < N_FILES; i++) {
    int fd = open(paths[i], O_RDONLY);
    if (fd >= 0) {
      posix_fadvise(fd, 0, FILE_BYTES, POSIX_FADV_DONTNEED);
      close(fd);
    }
  }
}

/* ------------------------------------------------------------------ */

int main(int argc, char** argv) {
  const char* dir = argc > 1 ? argv[1] : ".";

  printf("word count over %d files of %d bytes, page cache dropped per pass\n\n",
         N_FILES, FILE_BYTES);

  write_corpus(dir);

  int fd[N_FILES];
  unsigned char* buf[N_FILES];
  for (int i = 0; i < N_FILES; i++) {
    buf[i] = malloc(FILE_BYTES);
    memset(buf[i], 0, FILE_BYTES);
  }
  for (int i = 0; i < N_FILES; i++) {
    fd[i] = open(paths[i], O_RDONLY);
    if (fd[i] < 0) {
      printf("cannot open %s\n", paths[i]);
      return 1;
    }
  }

  /* Does this filesystem have device latency to overlap at all? A warm pass
   * against a dropped one answers it, and if the answer is no then the timing
   * comparison below is not meaningful and the demo says so instead of quoting
   * a ratio it cannot support. */
  double w0 = now_ms();
  for (int i = 0; i < N_FILES; i++)
    if (pread(fd[i], buf[i], FILE_BYTES, 0) != FILE_BYTES)
      return 1;
  double warm = now_ms() - w0;

  drop_caches();
  double c0 = now_ms();
  for (int i = 0; i < N_FILES; i++)
    if (pread(fd[i], buf[i], FILE_BYTES, 0) != FILE_BYTES)
      return 1;
  double cold = now_ms() - c0;

  int regime_ok = cold > warm * 1.5;
  printf("  warm:   %7.2f ms  (%5.2f us/file)\n", warm, warm * 1000.0 / N_FILES);
  printf("  cold:   %7.2f ms  (%5.2f us/file)  -> %.1fx\n\n", cold,
         cold * 1000.0 / N_FILES, cold / (warm > 0 ? warm : 1e-9));
  if (!regime_ok)
    printf("  !! this filesystem shows no device latency (tmpfs?), so the two arms\n"
           "     below read the same bytes at the same speed. Use a real disk.\n\n");

  /* ------------------------------------------------------------------ */
  /* Arm 1 -- blocking                                                   */
  /* ------------------------------------------------------------------ */
  drop_caches();
  size_t words_blocking = 0;

  double t1 = now_ms();
  for (int i = 0; i < N_FILES; i++) {
    if (pread(fd[i], buf[i], FILE_BYTES, 0) != FILE_BYTES)
      return 1;
    words_blocking += count_words((const char*)buf[i], FILE_BYTES);
  }
  double blocking_ms = now_ms() - t1;

  /* ------------------------------------------------------------------ */
  /* Arm 2 -- implicit async                                             */
  /* ------------------------------------------------------------------ */
  drop_caches();
  fasync_reset_stats();

  size_t words_implicit = 0;
  double t2 = now_ms();

  fasync_id ids[N_FILES];
  for (int i = 0; i < N_FILES; i++) {
    memset(buf[i], 0, FILE_BYTES);
    ids[i] = fasync_pread(fd[i], buf[i], FILE_BYTES, 0);
    if (!ids[i]) {
      printf("enqueue failed: %s\n", fasync_last_error());
      return 1;
    }
  }
  fasync_submit();
  double submit_ms = now_ms() - t2;

  /* No wait anywhere: each count resolves its own buffer at the first byte. */
  double t3 = now_ms();
  for (int i = 0; i < N_FILES; i++)
    words_implicit += count_words((const char*)buf[i], FILE_BYTES);
  double count_ms = now_ms() - t3;

  /* Bookkeeping only: the counts above are already done. */
  for (int i = 0; i < N_FILES; i++)
    fasync_result(ids[i]);

  double implicit_ms = submit_ms + count_ms;
  struct fasync_stats s;
  fasync_get_stats(&s);

  /* ------------------------------------------------------------------ */
  /* Report                                                              */
  /* ------------------------------------------------------------------ */
  printf("  blocking:  %7.2f ms   %zu words\n", blocking_ms, words_blocking);
  printf("  implicit:  %7.2f ms   %zu words   (%.2f to submit + %.2f to count)\n",
         implicit_ms, words_implicit, submit_ms, count_ms);
  if (regime_ok)
    printf("  speedup:   %7.2fx\n", blocking_ms / (implicit_ms > 0 ? implicit_ms : 1e-9));
  else
    printf("  speedup:   not measurable here (see above)\n");

  printf("\n  the two loops, from this file:\n\n"
         "    blocking        for (i) { pread(fd[i], buf[i], N, 0);\n"
         "                             words += count_words(buf[i], N); }\n\n"
         "    implicit async  for (i) { fasync_pread(fd[i], buf[i], N, 0); }\n"
         "                    fasync_submit();\n"
         "                    for (i) { words += count_words(buf[i], N); }\n\n"
         "  count_words() is the same function in both, and it contains no async\n"
         "  call, no marker and no await. What the second loop bought:\n\n");
  printf("    kernel submits: %lu for %d files (one batch)\n",
         s.kernel_submit_entries, N_FILES);
  printf("    kernel waits:   %lu\n", s.kernel_wait_entries);
  printf("    completions reaped in userspace: %lu\n", s.completions_reaped);

  printf("\n  On this device the win is bounded by how many of those reads the disk\n"
         "  can serve at once, not by the software: tests/stage9_device_parallelism.c\n"
         "  measures that ceiling, and stage8 puts the same workload's three arms\n"
         "  side by side. On a warm cache, or a filesystem with no latency to hide,\n"
         "  there is nothing here to win -- which is what the other measurements in\n"
         "  this repo honestly report.\n");

  check("both arms counted the same words", words_blocking == words_implicit);
  check("the words are not zero", words_implicit > 0);
  check("the compiler's hook resolved the buffers", s.resolve_calls > 0);
  check("submission never blocked", s.kernel_wait_entries == 0);
  check("one batch, not one syscall per file",
        s.kernel_submit_entries < (unsigned long)N_FILES);

  for (int i = 0; i < N_FILES; i++) {
    close(fd[i]);
    unlink(paths[i]);
    free(buf[i]);
  }

  printf("\nDEMO %s\n", failures ? "FAIL" : "OK");
  return failures ? 1 : 0;
}
