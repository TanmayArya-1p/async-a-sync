/* provenance: a write and a read of one file, through two fds.
 *
 * The two fds hide that the calls touch the same bytes, so nothing orders
 * them. A tracker shared by both calls puts the read after the write. */

#include "explicit_utils.hh"

// No tracker: the read may run before the write.
static struct pair untracked(struct provenance* p) {
  struct pair c;
  c.write = fasync_pwrite(p->fd_write, p->written, p->len, 0);
  c.read = fasync_pread(p->fd_read, p->read_untracked, p->len, 0);
  return c;
}

// One tracker for both calls: the read waits for the write.
static struct pair tracked(struct provenance* p) {
  fasync_tracker* t = fasync_tracker_new();
  struct pair c;
  c.write = fasync_tagged_pwrite(p->fd_write, p->written, p->len, 0, t, FASYNC_INOUT);
  c.read = fasync_tagged_pread(p->fd_read, p->read_tracked, p->len, 0, t, FASYNC_INOUT);
  fasync_tracker_free(t);
  return c;
}

int main(int argc, char** argv) {
  struct provenance p = provenance_setup(argc, argv);

  check_untracked(&p, untracked(&p));
  check_tracked(&p, tracked(&p));

  return provenance_report(&p);
}
