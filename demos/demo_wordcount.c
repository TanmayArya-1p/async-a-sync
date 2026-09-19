/*
 * demo_wordcount.c -- the same word count, written twice.
 *
 * The first loop reads each file and counts it. The second submits a read for
 * every file, then counts -- with no wait written anywhere. count_words() is shared
 * between them and has never heard of io_uring: the compiler's hook resolves each
 * buffer at the first byte it touches.
 *
 * The page cache is dropped before each pass, so a read costs a device round trip
 * and the two loops are worth comparing. Warm, they come out level.
 *
 * Build with the patched compiler, not the stock one:
 *   vendor/fil-c-src/build/bin/filcc -O2 -static -DFASYNC_COMPILER_INSERTS_CHECKS \
 *     -I runtime/src -L runtime/build/lib -o demo_wordcount demos/demo_wordcount.c
 *   ./demo_wordcount <dir on a real filesystem>     # not /tmp: that is tmpfs
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fasync.h"
#include "utils.hh"

#ifndef FASYNC_COMPILER_INSERTS_CHECKS
#error "build with the patched compiler and -DFASYNC_COMPILER_INSERTS_CHECKS"
#endif

#define FILES 512
#define BYTES 4096

static char paths[FILES][DEMO_PATH_MAX];
static unsigned char* buf[FILES];
static int fd[FILES];

int main(int argc, char** argv) {
  const char* dir = argc > 1 ? argv[1] : ".";

  printf("word count over %d files of %d KiB, page cache dropped per pass\n\n",
         FILES, BYTES / 1024);

  if (demo_make_corpus(dir, "wc", FILES, BYTES, paths, 0) < 0)
    return 1;
  for (int i = 0; i < FILES; i++) {
    buf[i] = malloc(BYTES);
    memset(buf[i], 0, BYTES);
    fd[i] = open(paths[i], O_RDONLY);
    if (fd[i] < 0)
      return 1;
  }

  /* Is there device latency here at all? */
  double warm = demo_read_pass(fd, buf, FILES, BYTES);
  demo_drop_caches(fd, FILES, BYTES);
  double cold = demo_read_pass(fd, buf, FILES, BYTES);
  if (cold < warm * 1.5)
    printf("  (this filesystem has no device latency, so there is nothing to "
           "overlap)\n\n");

  /* blocking: read each file, then count it */
  demo_drop_caches(fd, FILES, BYTES);
  size_t blocking_words = 0;
  double t0 = demo_now_ms();
  for (int i = 0; i < FILES; i++) {
    if (pread(fd[i], buf[i], BYTES, 0) != BYTES)
      return 1;
    blocking_words += demo_count_words((const char*)buf[i], BYTES);
  }
  double blocking_ms = demo_now_ms() - t0;

  /* implicit: submit every read, then count. no wait is written anywhere */
  demo_drop_caches(fd, FILES, BYTES);
  fasync_reset_stats();

  fasync_id ids[FILES];
  size_t implicit_words = 0;
  double t1 = demo_now_ms();
  for (int i = 0; i < FILES; i++) {
    memset(buf[i], 0, BYTES);
    ids[i] = fasync_pread(fd[i], buf[i], BYTES, 0);
    if (!ids[i])
      return 1;
  }
  fasync_submit();
  for (int i = 0; i < FILES; i++)
    implicit_words += demo_count_words((const char*)buf[i], BYTES);
  double implicit_ms = demo_now_ms() - t1;

  struct fasync_stats s;
  fasync_get_stats(&s);

  printf("  blocking   %7.2f ms   %zu words\n", blocking_ms, blocking_words);
  printf("  implicit   %7.2f ms   %zu words   (%.2fx)\n", implicit_ms,
         implicit_words, blocking_ms / (implicit_ms > 0 ? implicit_ms : 1e-9));
  printf("\n  %lu kernel submit for %d files, %lu waits\n\n",
         s.kernel_submit_entries, FILES, s.kernel_wait_entries);

  int bad = blocking_words != implicit_words || !implicit_words ||
            !s.resolve_calls || s.kernel_submit_entries >= (unsigned long)FILES;
  if (bad)
    printf("FAIL: %zu vs %zu words, %lu submits, %lu resolutions\n",
           blocking_words, implicit_words, s.kernel_submit_entries,
           s.resolve_calls);

  for (int i = 0; i < FILES; i++) {
    close(fd[i]);
    free(buf[i]);
  }
  demo_remove_corpus(paths, FILES);
  return bad;
}
