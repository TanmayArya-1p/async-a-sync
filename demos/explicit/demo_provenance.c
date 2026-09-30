/* provenance: a write and a read of one file, through two fds.
 *
 * The two fds hide that the calls touch the same bytes, so nothing orders
 * them. A tracker shared by both calls puts the read after the write. */

#include "provenance_utils.hh"

// No tracker: the read may run before the write.
static struct pair untracked(struct demo* d) {
  struct pair c;
  c.write = fasync_pwrite(d->fd_write, d->written, d->len, 0);
  c.read = fasync_pread(d->fd_read, d->read_untracked, d->len, 0);
  return c;
}

// One tracker for both calls: the read waits for the write.
static struct pair tracked(struct demo* d) {
  fasync_tracker* t = fasync_tracker_new();
  struct pair c;
  c.write = fasync_tagged_pwrite(d->fd_write, d->written, d->len, 0, t, FASYNC_INOUT);
  c.read = fasync_tagged_pread(d->fd_read, d->read_tracked, d->len, 0, t, FASYNC_INOUT);
  fasync_tracker_free(t);
  return c;
}

int main(int argc, char** argv) {
  struct demo d = setup(argc, argv);

  check_untracked(&d, untracked(&d));
  check_tracked(&d, tracked(&d));

  return report(&d);
}
