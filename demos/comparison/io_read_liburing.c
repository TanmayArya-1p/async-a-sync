/* io_read, with liburing.
 *
 * Reads every file, then checksums each one. Every read gets a submission
 * entry, one submit sends them all, and each completion names the file
 * whose buffer is ready to checksum.
 *
 * The same task with an annotated call: io_read_filc_async.c.
 * Files, timing and the result line: compare_utils.h. */

#include <liburing.h>

#include "compare_utils.h"

/* task */
static struct io_uring ring;

static void open_ring(unsigned entries) {
  int err = io_uring_queue_init(entries, &ring, 0);
  if (err < 0) {
    fprintf(stderr, "io_uring_queue_init: %s\n", strerror(-err));
    exit(1);
  }
}

static uint64_t read_all(struct files* f) {
  for (int i = 0; i < f->n; i++) {
    struct io_uring_sqe* sqe = io_uring_get_sqe(&ring);
    io_uring_prep_read(sqe, f->fd[i], f->buf[i], f->bytes, 0);
    io_uring_sqe_set_data64(sqe, i);
  }
  io_uring_submit(&ring);

  uint64_t sum = 0;
  for (int done = 0; done < f->n; done++) {
    struct io_uring_cqe* cqe;
    int err = io_uring_wait_cqe(&ring, &cqe);
    if (err < 0 || cqe->res != (int)f->bytes) {
      fprintf(stderr, "read failed: %d\n", err < 0 ? err : cqe->res);
      exit(1);
    }
    int i = (int)io_uring_cqe_get_data64(cqe);
    sum += fnv1a(f->buf[i], f->bytes);
    io_uring_cqe_seen(&ring, cqe);
  }
  return sum;
}
/* end task */

int main(int argc, char** argv) {
  struct bench b = setup(argc, argv, "io_read", "liburing");
  open_ring((unsigned)b.f.n);
  run_passes(&b, read_all);
  io_uring_queue_exit(&ring);
  return report(&b);
}
