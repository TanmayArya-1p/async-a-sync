/* demo_pragma_scaling: how the gain changes with the number of reads.
 *
 * Runs the three ways to read from pragma_reads.hh over the first N files,
 * for N from 1 to 2048, each pass from a dropped page cache.
 *
 * One read has nothing to overlap with, so the three should tie. As N grows
 * the blocking loop pays N device round trips, while the async versions pay
 * about one per batch. Above 1024 files the runtime's request table is full,
 * so annotated calls wait for room while the hand-written version works in
 * waves.
 *
 * Usage: demo_pragma_scaling [dir] [max files] [passes] */

#include "pragma_reads.hh"
#include "pragma_report.hh"

static const int sizes[] = {1, 4, 16, 64, 256, 512, 1024, 2048};

int main(int argc, char** argv) {
  struct timing t = scaling_setup(argc, argv);

  for (size_t s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++) {
    int n = sizes[s];
    if (n > t.files)
      break;
    for (int pass = 0; pass < t.passes; pass++) {
      time_pass(&t.blocking[pass], read_blocking, n);
      time_pass(&t.annotated[pass], read_annotated, n);
      time_pass(&t.by_hand[pass], read_by_hand, n);
    }
    scaling_row(&t, n);
  }

  return scaling_report(&t);
}
