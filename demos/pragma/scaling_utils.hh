#pragma once

// Setup, rows and report for demo_pragma_scaling.

#include "pragma_utils.hh"

struct demo {
  int files; // the most files
  int passes;
  int cache_matters;
  struct pass blocking[MAX_PASSES];
  struct pass annotated[MAX_PASSES];
  struct pass by_hand[MAX_PASSES];

  // running results
  int words_ok;
  int batched;
  double best;
  int best_n;
  double last_call_us;
  double last_speedup;
  int last_n;
};

static inline struct demo setup(int argc, char** argv) {
  struct demo d = {.files = 2048, .passes = 3, .words_ok = 1, .batched = 1};
  title("scaling: the gain as the number of reads grows", 0);
  read_args(argc, argv, &d.files, &d.passes);
  printf("  Word count over N cold files of %d bytes, N = 1..%d, median of %d\n"
         "  passes. us/call is the time inside each annotated call.\n\n",
         READ_BYTES, d.files, d.passes);
  d.cache_matters = prepare_files(argc, argv, d.files, READ_BYTES);
  printf("  %6s %12s %12s %12s %8s %8s\n", "files", "A pread ms",
         "B async ms", "C by hand ms", "B vs A", "us/call");
  return d;
}

// Prints the row for n files from the passes just run.
static inline void row(struct demo* d, int n) {
  double a = pass_median(d->blocking, d->passes, MS);
  double b = pass_median(d->annotated, d->passes, MS);
  double c = pass_median(d->by_hand, d->passes, MS);
  double call_us = pass_median(d->annotated, d->passes, ISSUE_MS) * 1000.0 / n;
  double speedup = ratio(a, b);
  printf("  %6d %12.2f %12.2f %12.2f %7.2fx %8.1f\n", n, a, b, c, speedup,
         call_us);

  d->words_ok &= words_counted(d->blocking, d->passes, n) &&
                 words_counted(d->annotated, d->passes, n) &&
                 words_counted(d->by_hand, d->passes, n);
  // Below 64 files a pass is too short for the ratio to mean much.
  if (n >= 64) {
    d->batched &= pass_median(d->annotated, d->passes, ENTRIES) <= n / 4.0 + 2;
    if (speedup > d->best) {
      d->best = speedup;
      d->best_n = n;
    }
  }
  d->last_call_us = call_us;
  d->last_speedup = speedup;
  d->last_n = n;
}

static inline int report(struct demo* d) {
  // Above 1024 files the request table is full, so us/call includes waiting
  // for room in it.
  if (d->best_n)
    printf("\n  => %.2fx faster than blocking at %d files, %.2fx at %d (%.1f us "
           "per call)\n",
           d->best, d->best_n, d->last_speedup, d->last_n, d->last_call_us);

  check("every pass counted every word", d->words_ok);
  check_bodies();
  check("from 64 files up, B batched its reads into few entries", d->batched);
  if (d->cache_matters && d->best_n)
    check("B beat A at some size from 64 up on an uncached device", d->best > 1.0);
  else
    printf("  (B vs A not checked: dropping the cache added no device latency)\n");

  remove_files();
  return finish();
}
