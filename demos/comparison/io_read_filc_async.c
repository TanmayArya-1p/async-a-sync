/* io_read, with an annotated call on our io_uring runtime.
 *
 * Reads every file, then checksums each one. async_pread queues a read and
 * returns at once; checksumming a buffer waits for its read. The reads
 * reach the kernel together, the first time a buffer is needed.
 *
 * The same task with liburing: io_read_liburing.c.
 * Files, timing and the result line: compare_utils.h. */

#include "filc_async.h"
#include "compare_utils.h"

/* task */
FILC_ASYNC(io_uring, FILC_OP(pread), FILC_BOUT(buf))
void* async_pread(int fd, void* buf, size_t len, unsigned long offset) {
  return 0;
}

static uint64_t read_all(struct files* f) {
  for (int i = 0; i < f->n; i++)
    async_pread(f->fd[i], f->buf[i], f->bytes, 0); // queues the read
  uint64_t sum = 0;
  for (int i = 0; i < f->n; i++)
    sum += fnv1a(f->buf[i], f->bytes); // waits for that read
  return sum;
}
/* end task */

int main(int argc, char** argv) {
  struct bench b = setup(argc, argv, "io_read", "FILC_ASYNC");
  run_passes(&b, read_all);
  return report(&b);
}
