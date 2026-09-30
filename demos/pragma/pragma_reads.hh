#pragma once

/* Three ways to read the first n demo files and count their words, compared
 * by demo_pragma_coldread and demo_pragma_scaling.
 *
 * read_blocking and read_annotated are the same two loops; only the read call
 * differs. read_annotated keeps no handles and has no wait: wordcount's first
 * read of each buffer waits for that buffer's read. read_by_hand does the
 * same work against the explicit fasync API, as the reference for what the
 * annotation costs next to hand-written async code. */

#include "pragma_io.hh"

static size_t read_blocking(int n) {
  for (int i = 0; i < n; i++)
    pread(demo_fd[i], demo_buf[i], demo_bytes, 0);

  size_t words = 0;
  for (int i = 0; i < n; i++)
    words += wordcount((const char*)demo_buf[i]);
  return words;
}

static size_t read_annotated(int n) {
  for (int i = 0; i < n; i++)
    async_pread(demo_fd[i], demo_buf[i], demo_bytes, 0);
  pragma_mark_issued();

  size_t words = 0;
  for (int i = 0; i < n; i++)
    words += wordcount((const char*)demo_buf[i]);
  return words;
}

/* The request table holds 1024 requests, so the hand-written version works
 * in waves: queue, submit, then wait for each handle. */
#define WAVE 512

static size_t read_by_hand(int n) {
  size_t words = 0;
  fasync_id ids[WAVE];
  for (int base = 0; base < n; base += WAVE) {
    int wave = n - base < WAVE ? n - base : WAVE;
    for (int i = 0; i < wave; i++)
      ids[i] = fasync_pread(demo_fd[base + i], demo_buf[base + i], demo_bytes, 0);
    fasync_submit();
    for (int i = 0; i < wave; i++) {
      fasync_result(ids[i]);
      words += wordcount((const char*)demo_buf[base + i]);
    }
  }
  return words;
}
