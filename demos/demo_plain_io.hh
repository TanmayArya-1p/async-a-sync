/*
 * demo_plain_io.hh -- the crux: request every file, then count every file, one
 * synchronous-looking loop. In the implicit build the requests only enqueue --
 * the first buffer access is where the queue reaches the kernel, lazily, as
 * one batch. No submit call, no wait call: this loop is all there is.
 *
 * The run file (demo_plain_io.c) creates the files, drops the page cache, and
 * prints the verdict and the mechanism counters.
 */
#pragma once

#include "utils.hh"

static int demo_plain_io(void) {
  /* Request one async read per file; nothing waits on any of them. */
  for (int i = 0; i < demo_n; i++)
    if (!fasync_pread(demo_fd[i], demo_buf[i], demo_bytes, 0))
      return 0;

  /* Count each file; reading the buffer is what resolves its read. */
  int correct = 0;
  for (int i = 0; i < demo_n; i++)
    if (wordcount((const char*)demo_buf[i]) == demo_expect[i])
      correct++;
  return correct;
}