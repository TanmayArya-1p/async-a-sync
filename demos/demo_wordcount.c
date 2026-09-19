/*
 * demo_wordcount.c -- the same word count, written two ways.
 *
 * The sync loop reads each file and counts it. The implicit loop submits a read
 * for every file, then counts: the count is where the wait happens, and it is
 * invisible because the compiler resolves each buffer at the first byte it
 * touches. count_words() (in utils.hh) is shared and has no async anything.
 *
 * Run it on a real filesystem -- build/tests works, /tmp is tmpfs and will show
 * 1.0x.
 *
 *   vendor/fil-c-src/build/bin/filcc -O2 -static -DFASYNC_COMPILER_INSERTS_CHECKS \
 *     -I runtime/src -L runtime/build/lib -o demo_wordcount demos/demo_wordcount.c
 *   ./demo_wordcount build/tests
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

int main(int argc, char** argv) {
  const char* dir = argc > 1 ? argv[1] : ".";

  char paths[FILES][DEMO_PATH_MAX];
  if (demo_make_corpus(dir, "wc", FILES, BYTES, paths, 0) < 0)
    return 1;

  int fd[FILES];
  unsigned char* buf[FILES];
  for (int i = 0; i < FILES; i++) {
    buf[i] = malloc(BYTES);
    memset(buf[i], 0, BYTES);
    fd[i] = open(paths[i], O_RDONLY);
  }

  /* sync: read each file, then count it */
  demo_drop_caches(fd, FILES, BYTES);
  size_t sync_words = 0;
  double t0 = demo_now_ms();
  for (int i = 0; i < FILES; i++) {
    if (pread(fd[i], buf[i], BYTES, 0) != BYTES)
      return 1;
    sync_words += demo_count_words((const char*)buf[i], BYTES);
  }
  double sync_ms = demo_now_ms() - t0;

  /* implicit: submit every read, then count -- the count is the wait */
  demo_drop_caches(fd, FILES, BYTES);
  size_t implicit_words = 0;
  double t1 = demo_now_ms();
  for (int i = 0; i < FILES; i++) {
    memset(buf[i], 0, BYTES);
    if (!fasync_pread(fd[i], buf[i], BYTES, 0))
      return 1;
  }
  fasync_submit();
  for (int i = 0; i < FILES; i++)
    implicit_words += demo_count_words((const char*)buf[i], BYTES);
  double implicit_ms = demo_now_ms() - t1;

  printf("  sync       %7.2f ms\n", sync_ms);
  printf("  implicit   %7.2f ms   (%.2fx)\n", implicit_ms,
         sync_ms / (implicit_ms > 0 ? implicit_ms : 1e-9));

  for (int i = 0; i < FILES; i++) {
    close(fd[i]);
    free(buf[i]);
  }
  demo_remove_corpus(paths, FILES);

  return sync_words != implicit_words;
}
