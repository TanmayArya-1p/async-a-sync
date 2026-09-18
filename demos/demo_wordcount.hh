/*
 * demo_wordcount.hh -- the crux: ask for every file, then count every file.
 * Built and run two ways by demos/run_wordcount.sh:
 *
 *   sync:     no async runtime; read_all_files() blocks each read.
 *   implicit: patched Fil-C + io_uring; read_all_files() returns before the
 *             reads finish, and counting a file is what waits. There is no
 *             submit and no wait anywhere -- the first access to a buffer
 *             publishes the whole queue lazily.
 *
 * The source that runs is identical in both. The flag decides the backend.
 * The run file (demo_wordcount.c) creates the files, times the run, and turns
 * the tally into a verdict. Run on a real disk: on tmpfs a read costs nothing
 * and the ratio is 1.0x.
 */
#pragma once

#include "utils.hh"

static size_t demo_wordcount(void) {
  /* Ask for every file, without waiting for any of them. */
  read_all_files();

  /* Count each file; touching a buffer is what waits. */
  size_t words = 0;
  for (int i = 0; i < demo_n; i++)
    words += wordcount(file_data(i));
  return words;
}