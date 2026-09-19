/*
 * demo_wordcount.c -- the same word count, written twice.
 *
 * Both loops count words in the same 512 files. The first reads each file with
 * pread(); the second submits a read for every file and then just counts, with no
 * wait written anywhere. count_words() is shared between them and has never heard
 * of io_uring -- the compiler's hook resolves each buffer at the first byte it
 * touches.
 *
 * The page cache is dropped before each pass, so every read costs a device round
 * trip: a serial program pays one per file, an overlapped one pays about one for
 * the batch. On a warm cache there is nothing to overlap and the two come out
 * level, which is what the other measurements in this repo report.
 *
 * Build and run (needs the patched compiler, vendor/fil-c-src/build/bin/filcc):
 *   vendor/fil-c-src/build/bin/filcc -O2 -static \
 *     -DFASYNC_COMPILER_INSERTS_CHECKS -I runtime/src -L runtime/build/lib \
 *     -o build/tests/demo_wordcount demos/demo_wordcount.c
 *   ./build/tests/demo_wordcount build/tests
 *
 * The directory argument is where the corpus is written, and it wants to be on a
 * real filesystem: /tmp is usually tmpfs, which has no device latency to overlap.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>

#include "fasync.h"

/* Without the compiler-inserted hook the second loop has nothing to resolve its
 * buffers and would silently count zeros. */
#ifndef FASYNC_COMPILER_INSERTS_CHECKS
#error "build with the patched compiler and -DFASYNC_COMPILER_INSERTS_CHECKS"
#endif

#define N_FILES 512
#define FILE_BYTES 4096

static char paths[N_FILES][128];
static unsigned char* buf[N_FILES];
static int fd[N_FILES];

/* The ordinary code. One implementation, used by both loops. */
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

static double now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

static void drop_caches(void) {
  for (int i = 0; i < N_FILES; i++)
    posix_fadvise(fd[i], 0, FILE_BYTES, POSIX_FADV_DONTNEED);
}

int main(int argc, char** argv) {
  const char* dir = argc > 1 ? argv[1] : ".";
  static const char* words[] = {"alpha", "beta", "gamma", "delta", "epsilon"};

  srand(7);
  for (int i = 0; i < N_FILES; i++) {
    unsigned char* b = malloc(FILE_BYTES);
    size_t n = 0;
    while (n < FILE_BYTES) {
      const char* w = words[rand() % 5];
      size_t wl = strlen(w);
      if (n + wl + 1 > FILE_BYTES)
        break;
      memcpy(b + n, w, wl);
      n += wl;
      b[n++] = (rand() % 8) ? ' ' : '\n';
    }
    memset(b + n, ' ', FILE_BYTES - n);

    snprintf(paths[i], sizeof(paths[i]), "%s/wc_%04d.txt", dir, i);
    int w = open(paths[i], O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (w < 0 || pwrite(w, b, FILE_BYTES, 0) != FILE_BYTES) {
      printf("cannot write %s\n", paths[i]);
      return 1;
    }
    fsync(w);
    close(w);
    free(b);

    buf[i] = malloc(FILE_BYTES);
    memset(buf[i], 0, FILE_BYTES);
    fd[i] = open(paths[i], O_RDONLY);
    if (fd[i] < 0)
      return 1;
  }

  printf("word count over %d files of %d KiB, page cache dropped per pass\n\n",
         N_FILES, FILE_BYTES / 1024);

  /* Is there device latency here at all? */
  double t = now_ms();
  for (int i = 0; i < N_FILES; i++)
    if (pread(fd[i], buf[i], FILE_BYTES, 0) != FILE_BYTES)
      return 1;
  double warm = now_ms() - t;
  drop_caches();
  t = now_ms();
  for (int i = 0; i < N_FILES; i++)
    if (pread(fd[i], buf[i], FILE_BYTES, 0) != FILE_BYTES)
      return 1;
  double cold = now_ms() - t;
  if (cold < warm * 1.5)
    printf("  (this filesystem has no device latency, so there is nothing to "
           "overlap)\n\n");

  /* blocking: read each file, then count it */
  drop_caches();
  size_t blocking_words = 0;
  double t0 = now_ms();
  for (int i = 0; i < N_FILES; i++) {
    if (pread(fd[i], buf[i], FILE_BYTES, 0) != FILE_BYTES)
      return 1;
    blocking_words += count_words((const char*)buf[i], FILE_BYTES);
  }
  double blocking_ms = now_ms() - t0;

  /* implicit: submit every read, then count. no wait is written anywhere */
  drop_caches();
  fasync_reset_stats();

  fasync_id ids[N_FILES];
  size_t implicit_words = 0;
  double t1 = now_ms();
  for (int i = 0; i < N_FILES; i++) {
    memset(buf[i], 0, FILE_BYTES);
    ids[i] = fasync_pread(fd[i], buf[i], FILE_BYTES, 0);
    if (!ids[i]) {
      printf("enqueue failed: %s\n", fasync_last_error());
      return 1;
    }
  }
  fasync_submit();
  for (int i = 0; i < N_FILES; i++)
    implicit_words += count_words((const char*)buf[i], FILE_BYTES);
  double implicit_ms = now_ms() - t1;

  for (int i = 0; i < N_FILES; i++)
    fasync_result(ids[i]);

  struct fasync_stats s;
  fasync_get_stats(&s);

  printf("  blocking   %7.2f ms   %zu words\n", blocking_ms, blocking_words);
  printf("  implicit   %7.2f ms   %zu words   (%.2fx)\n", implicit_ms,
         implicit_words, blocking_ms / (implicit_ms > 0 ? implicit_ms : 1e-9));
  printf("\n  %lu kernel submit for %d files, %lu waits\n\n",
         s.kernel_submit_entries, N_FILES, s.kernel_wait_entries);

  int bad = blocking_words != implicit_words || !implicit_words ||
            !s.resolve_calls || s.kernel_submit_entries >= (unsigned long)N_FILES;
  if (bad) {
    printf("FAIL: %zu vs %zu words, %lu submits, %lu resolutions\n",
           blocking_words, implicit_words, s.kernel_submit_entries,
           s.resolve_calls);
    return 1;
  }

  for (int i = 0; i < N_FILES; i++) {
    close(fd[i]);
    unlink(paths[i]);
    free(buf[i]);
  }
  return 0;
}
