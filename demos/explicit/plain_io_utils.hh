#pragma once

// Setup and report for demo_plain_io.

#include "explicit_utils.hh"

static inline void setup(int argc, char** argv) {
  title("plain_io: ask for every file, count every file",
        "  fasync_pread queues each read. Counting a buffer waits for it.");
  make_files(dir_arg(argc, argv), 4, 512 * 1024);
  drop_cache();
}

static inline int report(int correct) {
  struct fasync_stats s = io_stats();
  printf("  %d files of %zu KiB: %lu reads queued, %lu kernel entries\n",
         files.n, files.bytes / 1024, s.sqes_queued,
         s.kernel_submit_entries + s.kernel_wait_entries);
  printf("  %d/%d files counted right\n\n", correct, files.n);
  check("every file counted right", correct == files.n);
  remove_files();
  return finish();
}
