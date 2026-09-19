/*
 * demo_plain_io.hh -- four reads, no wait, then count.
 *
 * The buffers go to count_words() while the kernel is still filling them, and it
 * cannot tell. The per-file counters show the range memo doing the resolution
 * work. These files are cache-resident on purpose; demo_wordcount.hh is the one
 * where overlapping pays.
 */
#ifndef DEMO_PLAIN_IO_HH
#define DEMO_PLAIN_IO_HH

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fasync.h"
#include "utils.hh"

#ifndef FASYNC_COMPILER_INSERTS_CHECKS
#error "build with the patched compiler and -DFASYNC_COMPILER_INSERTS_CHECKS"
#endif

#define FILES 4
#define BYTES (512 * 1024)

static int failures = 0;

static void check(const char* what, int ok) {
  printf("  %-56s %s\n", what, ok ? "ok" : "FAIL");
  if (!ok)
    failures++;
}

static int demo_plain_io(void) {
  char paths[FILES][DEMO_PATH_MAX];
  size_t expect[FILES];
  unsigned char* buf[FILES];
  int fd[FILES];

  printf("synchronous-looking code, run asynchronously\n");
  printf("%d files of %d KiB\n\n", FILES, BYTES / 1024);

  if (demo_make_corpus("/tmp", "plain_io", FILES, BYTES, paths, expect) < 0)
    return 1;
  for (int i = 0; i < FILES; i++) {
    buf[i] = malloc(BYTES);
    memset(buf[i], 0, BYTES);
    fd[i] = open(paths[i], O_RDONLY);
    if (fd[i] < 0)
      return 1;
  }

  /* Submit everything, wait for nothing. */
  fasync_reset_stats();
  double t0 = demo_now_ms();
  fasync_id id[FILES];
  for (int i = 0; i < FILES; i++) {
    id[i] = fasync_pread(fd[i], buf[i], BYTES, 0);
    if (!id[i])
      return 1;
  }
  fasync_submit();
  double submit_ms = demo_now_ms() - t0;

  struct fasync_stats s;
  fasync_get_stats(&s);
  printf("  submitted %lu SQEs in %.2f ms, in %lu kernel entry, %lu of which "
         "blocked\n", s.sqes_queued, submit_ms, s.kernel_submit_entries,
         s.kernel_wait_entries);
  printf("  completions reaped so far: %lu -- nothing has waited yet\n\n",
         s.completions_reaped);

  /* The ordinary part: no async call below, and no marker. */
  printf("  counting words with an ordinary function:\n\n");
  int all_correct = 1;
  double t1 = demo_now_ms();

  for (int i = 0; i < FILES; i++) {
    struct fasync_stats before, after;
    fasync_get_stats(&before);
    size_t words = demo_count_words((const char*)buf[i], BYTES);
    fasync_get_stats(&after);

    int correct = (words == expect[i]);
    if (!correct)
      all_correct = 0;

    printf("    %-22s %7zu words  %s\n", paths[i], words,
           correct ? "correct" : "WRONG");
    printf("      %lu of %d accesses entered the slow path, %lu answered by the "
           "range memo\n", after.resolve_calls - before.resolve_calls, BYTES,
           after.memo_hits - before.memo_hits);
  }
  double count_ms = demo_now_ms() - t1;

  /* The same work the blocking way. */
  unsigned char* cbuf = malloc(BYTES);
  int blocking_correct = 1;
  double t2 = demo_now_ms();
  for (int i = 0; i < FILES; i++) {
    if (pread(fd[i], cbuf, BYTES, 0) != BYTES)
      blocking_correct = 0;
    if (demo_count_words((const char*)cbuf, BYTES) != expect[i])
      blocking_correct = 0;
  }
  double blocking_ms = demo_now_ms() - t2;

  struct fasync_stats end;
  fasync_get_stats(&end);

  printf("\n  blocking: read + count per file   %7.2f ms\n", blocking_ms);
  printf("  async:    submit %.2f + count %.2f = %7.2f ms\n\n", submit_ms,
         count_ms, submit_ms + count_ms);

  check("the async path read the right bytes", all_correct);
  check("the blocking path agrees with it", blocking_correct);
  check("the compiler's hook resolved a buffer", end.resolve_calls > 0);
  check("the range memo answered the repeats", end.memo_hits > 0);
  check("submission never blocked", s.kernel_wait_entries == 0);

  /* Bookkeeping: the counts above are already done. */
  long total = 0;
  for (int i = 0; i < FILES; i++)
    total += fasync_result(id[i]);
  check("every read reported a full buffer", total == (long)FILES * BYTES);

  for (int i = 0; i < FILES; i++) {
    close(fd[i]);
    free(buf[i]);
  }
  free(cbuf);
  demo_remove_corpus(paths, FILES);

  printf("\nDEMO %s\n", failures ? "FAIL" : "OK");
  return failures ? 1 : 0;
}

#endif /* DEMO_PLAIN_IO_HH */
