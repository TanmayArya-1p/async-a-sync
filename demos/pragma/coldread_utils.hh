#pragma once

// Setup and report for demo_pragma_coldread.

#include "pragma_utils.hh"

struct demo {
  int files;
  int passes;
  int cache_matters;
  struct pass blocking[MAX_PASSES];
  struct pass annotated[MAX_PASSES];
  struct pass by_hand[MAX_PASSES];
};

static inline struct demo setup(int argc, char** argv) {
  struct demo d = {.files = 512, .passes = 5};
  title("coldread: the same loop, blocking and annotated", 0);
  read_args(argc, argv, &d.files, &d.passes);
  printf("  Word count over %d cold files of %d bytes, median of %d passes.\n"
         "  B is A's loop with pread renamed to async_pread.\n\n",
         d.files, READ_BYTES, d.passes);
  d.cache_matters = prepare_files(argc, argv, d.files, READ_BYTES);
  return d;
}

static inline int report(struct demo* d) {
  static const char* const name[] = {"A blocking pread",
                                     "B annotated async_pread",
                                     "C hand-written fasync_*"};
  const struct pass* way[] = {d->blocking, d->annotated, d->by_hand};
  double ms[3];
  for (int w = 0; w < 3; w++)
    ms[w] = pass_median(way[w], d->passes, MS);

  // kernel entries: one per pread for A; io_uring_enter submits and waits
  // for B and C. sleeps: times this thread blocked.
  printf("  %-26s %8s %7s %15s %7s\n", "arm", "ms", "vs A", "kernel entries",
         "sleeps");
  for (int w = 0; w < 3; w++) {
    double entries = w == 0 ? d->files : pass_median(way[w], d->passes, ENTRIES);
    printf("  %-26s %8.2f %6.2fx %15.0f %7.0f\n", name[w], ms[w],
           ratio(ms[0], ms[w]), entries, pass_median(way[w], d->passes, SLEEPS));
  }
  printf("\n  => %.2fx faster than blocking pread over %d cold files, same "
         "loop\n",
         ratio(ms[0], ms[1]), d->files);

  int words_ok = 1;
  for (int w = 0; w < 3; w++)
    words_ok &= words_counted(way[w], d->passes, d->files);
  check("every pass of every arm counted every word", words_ok);
  check_bodies();
  check("B entered the kernel far less than once per file",
        pass_median(d->annotated, d->passes, ENTRIES) < d->files / 4.0 + 1);
  if (d->cache_matters)
    check("B beat A on an uncached device", ms[1] < ms[0]);
  else
    printf("  (B vs A not checked: dropping the cache added no device latency)\n");

  remove_files();
  return finish();
}
