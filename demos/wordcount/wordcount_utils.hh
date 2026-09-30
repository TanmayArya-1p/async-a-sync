#pragma once

// Setup and report for demo_wordcount, so the demo file holds only the
// read and count loops.

#include "../common/utils.hh"

#ifdef FASYNC_IMPLICIT
#define MODE "implicit"
#else
#define MODE "sync"
#endif

static double wordcount_t0;

// 512 files of random words in dir, dropped from the page cache.
static inline void setup(int argc, char** argv) {
  make_files(argc > 1 ? argv[1] : "/tmp", 512, 4096);
  drop_cache();
  wordcount_t0 = now_ms();
}

// Prints "  <mode> <ms> ms  (<words> words)"; fails on a wrong count.
static inline int report(size_t words) {
  double ms = now_ms() - wordcount_t0;
  printf("  %-9s %7.2f ms  (%zu words)\n", MODE, ms, words);
  int ok = words == words_in_files(files.n);
  remove_files();
  return !ok;
}
