/* demo_pragma_coldread: the same loop, blocking and annotated.
 *
 * Counts the words in 512 files, each pass from a dropped page cache so every
 * read is a device round trip. The three ways to read are in pragma_reads.hh:
 *
 *   A  read_blocking   pread in a loop, then count
 *   B  read_annotated  the same loop calling async_pread
 *   C  read_by_hand    the same work against the explicit fasync API
 *
 * The passes are interleaved; pragma_report.hh prints the medians.
 *
 * Usage: demo_pragma_coldread [dir] [files] [passes] */

#include "pragma_reads.hh"
#include "pragma_report.hh"

int main(int argc, char** argv) {
  struct timing t = coldread_setup(argc, argv);

  for (int pass = 0; pass < t.passes; pass++) {
    time_pass(&t.blocking[pass], read_blocking, t.files);
    time_pass(&t.annotated[pass], read_annotated, t.files);
    time_pass(&t.by_hand[pass], read_by_hand, t.files);
  }

  return coldread_report(&t);
}
