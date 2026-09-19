/*
 * demo_plain_io.c -- synchronous-looking code, run asynchronously.
 *
 * Read count_words() below. It is ordinary C: no await, no future, no yield, no
 * marker macro. It cannot tell whether the bytes it walks are resident or still in
 * flight, and it is not supposed to care. The reads were submitted before it was
 * called, and the compiler's instrumentation resolves a buffer at the first byte
 * touched (see compiler/README.md).
 *
 * The only async-looking lines in this file are the four fasync_pread() calls. After
 * that, every access is a plain array index.
 *
 * This is not a performance demo, but it is the measurement that found a real
 * cost in the resolve fast path and then showed it fixed. The fast path is a check
 * that nothing at all is in flight, so with a batch outstanding every instrumented
 * access enters the slow path instead -- and this program walks bytes, so it enters
 * it once per byte. The first version of this demo measured 42 ms against an 8 ms
 * blocking baseline; a memo of ranges proved empty of pending buffers brought it
 * level, and the output prints both the slow-path count and how many of those the
 * memo answered. docs/ARCHITECTURE.md sections 7 and 8 carry the numbers.
 *
 * Build (needs the patched compiler -- see tests/run.sh, which skips this when that
 * compiler has not been built):
 *   vendor/fil-c-src/build/bin/filcc -O2 -static -DFASYNC_COMPILER_INSERTS_CHECKS \
 *     -I runtime/src -L runtime/build/lib -o demo_plain_io demos/demo_plain_io.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>

#include "fasync.h"

/*
 * Without the patched compiler there is nothing here to demonstrate: nothing would
 * ever resolve the buffers, and the word counts would silently come out zero. Fail
 * at build time instead.
 */
#ifndef FASYNC_COMPILER_INSERTS_CHECKS
#error "build with the patched compiler and -DFASYNC_COMPILER_INSERTS_CHECKS"
#endif

#define N_FILES 4
#define FILE_BYTES (512 * 1024)

static int failures = 0;

static void check(const char* what, int ok) {
  printf("  %-58s %s\n", what, ok ? "ok" : "FAIL");
  if (!ok)
    failures++;
}

static double now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

/* ------------------------------------------------------------------ */
/* The ordinary code                                                   */
/* ------------------------------------------------------------------ */

/*
 * An ordinary function.
 *
 * It takes a pointer and a length and walks them. Nothing in it is async-aware, and
 * nothing in it could be: resolution happens in the instrumentation the compiler put
 * in front of the load, which this function neither knows nor needs to know about.
 *
 * noinline is deliberate. Inlined, the access would land back in main next to the
 * submission and the demo would not be testing anything -- the claim is that an
 * ordinary *callee* can be handed a buffer that is still in flight.
 */
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
/* Test data                                                           */
/* ------------------------------------------------------------------ */

static const char* WORD_LIST[] = {"alpha", "beta", "gamma", "delta",
                                  "epsilon", "zeta", "eta", "theta"};
#define NWORDS ((int)(sizeof(WORD_LIST) / sizeof(WORD_LIST[0])))

static unsigned int g_rand = 12345;

static unsigned int next_rand(void) {
  g_rand = g_rand * 1103515245u + 12345u;
  return g_rand >> 16;
}

/*
 * Fill a buffer with words, remembering how many were written.
 *
 * The expected count is therefore known without trusting either path: the generator
 * counted what it emitted, so a path that reads the wrong bytes -- or reads zeros --
 * will not agree with it.
 */
static size_t gen_text(unsigned char* p, size_t cap) {
  size_t n = 0;
  size_t words = 0;

  while (n < cap) {
    const char* w = WORD_LIST[next_rand() % NWORDS];
    size_t wl = strlen(w);

    if (words) {
      if (n + 1 + wl > cap)
        break;
      p[n++] = (next_rand() & 8) ? '\n' : ' ';
    } else if (wl > cap) {
      break;
    }

    memcpy(p + n, w, wl);
    n += wl;
    words++;
  }

  /* Padding is spaces: not a word, so the count above stands and every read is a
   * full-length read. */
  memset(p + n, ' ', cap - n);
  return words;
}

/* ------------------------------------------------------------------ */

int main(void) {
  printf("synchronous-looking code, run asynchronously\n");
  printf("%d files of %d KiB\n\n", N_FILES, FILE_BYTES / 1024);

  char paths[N_FILES][64];
  size_t true_words[N_FILES];
  unsigned char* seed = malloc(FILE_BYTES);

  for (int i = 0; i < N_FILES; i++) {
    snprintf(paths[i], sizeof(paths[i]), "/tmp/async-a-sync_plain_%d.txt", i);
    true_words[i] = gen_text(seed, FILE_BYTES);
    int fd = open(paths[i], O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (fd < 0 || pwrite(fd, seed, FILE_BYTES, 0) != (ssize_t)FILE_BYTES) {
      printf("cannot write %s\n", paths[i]);
      return 1;
    }
    close(fd);
  }
  free(seed);

  int fd[N_FILES];
  unsigned char* buf[N_FILES];
  for (int i = 0; i < N_FILES; i++) {
    fd[i] = open(paths[i], O_RDONLY);
    if (fd[i] < 0)
      return 1;
    /* Heap buffers on purpose: the pass instruments accesses through escaping
     * pointers, which is what a malloc'd buffer is. */
    buf[i] = malloc(FILE_BYTES);
    memset(buf[i], 0, FILE_BYTES);
  }

  /* ------------------------------------------------------------------ */
  /* Submit, and do not wait                                             */
  /* ------------------------------------------------------------------ */
  fasync_reset_stats();

  double t_submit = now_ms();
  fasync_id ids[N_FILES];
  for (int i = 0; i < N_FILES; i++) {
    ids[i] = fasync_pread(fd[i], buf[i], FILE_BYTES, 0);
    if (!ids[i]) {
      printf("enqueue failed: %s\n", fasync_last_error());
      return 1;
    }
  }
  fasync_submit();
  double submit_ms = now_ms() - t_submit;

  struct fasync_stats s;
  fasync_get_stats(&s);

  printf("  submitted %lu SQEs in %.3f ms, in %lu kernel entry/entries, %lu of "
         "which blocked\n",
         s.sqes_queued, submit_ms, s.kernel_submit_entries, s.kernel_wait_entries);
  printf("  completions reaped so far: %lu -- nothing has waited yet\n\n",
         s.completions_reaped);

  /* ------------------------------------------------------------------ */
  /* The ordinary part                                                   */
  /* ------------------------------------------------------------------ */
  printf("  counting words. there is no async call below, and no marker:\n\n");

  int all_correct = 1;
  int resolved_here_count = 0;
  double t_count = now_ms();

  for (int i = 0; i < N_FILES; i++) {
    struct fasync_stats before, after;
    fasync_get_stats(&before);

    size_t words = count_words((const char*)buf[i], FILE_BYTES);

    fasync_get_stats(&after);

    int correct = (words == true_words[i]);
    if (!correct)
      all_correct = 0;

    /* Did the compiler's hook have to do anything at this access? */
    int resolved_here = after.resolve_calls > before.resolve_calls;
    if (resolved_here)
      resolved_here_count++;

    char how[96];
    if (resolved_here) {
      snprintf(how, sizeof(how),
               "resolved here (drained %lu, spins %lu, slept %lu)",
               after.completions_reaped - before.completions_reaped,
               after.spin_rounds - before.spin_rounds, after.parks - before.parks);
    } else {
      snprintf(how, sizeof(how), "fast path (nothing left in flight)");
    }

    printf("    %-34s %7zu words  %-7s %s\n", paths[i], words,
           correct ? "correct" : "WRONG", how);

    /*
     * Every byte of the loop above is an instrumented access, so this is how many
     * of them had to enter the resolve slow path, and how many of those the range
     * memo answered without walking the request table. It is the number that
     * decides what this demo costs.
     */
    unsigned long slow = after.resolve_calls - before.resolve_calls;
    unsigned long memo = after.memo_hits - before.memo_hits;
    printf("      %lu of %d accesses entered the slow path; %lu of those were "
           "answered by the range memo\n",
           slow, FILE_BYTES, memo);
  }

  double count_ms = now_ms() - t_count;

  /* ------------------------------------------------------------------ */
  /* The same work, the blocking way                                     */
  /* ------------------------------------------------------------------ */
  unsigned char* cbuf = malloc(FILE_BYTES);
  int blocking_correct = 1;
  double t_block = now_ms();

  for (int i = 0; i < N_FILES; i++) {
    if (pread(fd[i], cbuf, FILE_BYTES, 0) != (ssize_t)FILE_BYTES)
      blocking_correct = 0;
    if (count_words((const char*)cbuf, FILE_BYTES) != true_words[i])
      blocking_correct = 0;
  }

  double blocking_ms = now_ms() - t_block;

  /* ------------------------------------------------------------------ */
  /* Report                                                              */
  /* ------------------------------------------------------------------ */
  printf("\n");
  printf("  counts correct (async):    %s\n", all_correct ? "yes" : "NO");
  printf("  counts correct (blocking): %s\n", blocking_correct ? "yes" : "NO");
  printf("  accesses that resolved:    %d of %d -- the first one drains every "
         "completion that\n"
         "                             has landed, so the rest find nothing "
         "pending\n",
         resolved_here_count, N_FILES);

  printf("\n");
  printf("  blocking: read + count, per file   %7.2f ms\n", blocking_ms);
  printf("  async:    submit %.2f + count %.2f = %7.2f ms\n", submit_ms, count_ms,
         submit_ms + count_ms);
  printf("            (the count is itself the overlap: while one file is being\n"
         "             counted, the reads for the others are still in flight)\n");

  printf("\n  The ergonomics are the point, and they work: count_words() had no way to\n"
         "  tell that those buffers were in flight, and never had to say so.\n"
         "\n"
         "  The cost is the other half. The resolve fast path is a check that nothing\n"
         "  at all is in flight, so with a batch outstanding every access enters the\n"
         "  slow path -- and the slow path walked the request table. When this demo was\n"
         "  first written that was 42 ms against an 8 ms blocking baseline, one walk\n"
         "  per byte. A memo of the ranges proved to hold no pending buffer answers the\n"
         "  repeats without the walk, and the two columns above are now level.\n"
         "  docs/ARCHITECTURE.md sections 7 and 8 have the before and after.\n\n");

  check("the async path read the right bytes", all_correct);
  check("the blocking path agrees with it", blocking_correct);
  check("the compiler's hook fired at an access", resolved_here_count > 0);
  check("submission never blocked", s.kernel_wait_entries == 0);

  /* Bookkeeping only: the counts above are already computed. This is what would
   * report a short read, and it releases the request slots. */
  long total = 0;
  for (int i = 0; i < N_FILES; i++)
    total += fasync_result(ids[i]);
  check("every read reported a full buffer",
        total == (long)N_FILES * (long)FILE_BYTES);

  for (int i = 0; i < N_FILES; i++) {
    close(fd[i]);
    free(buf[i]);
    unlink(paths[i]);
  }
  free(cbuf);

  printf("\nDEMO %s\n", failures ? "FAIL" : "OK");
  return failures ? 1 : 0;
}
