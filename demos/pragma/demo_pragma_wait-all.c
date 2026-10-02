/* demo_pragma_wait-all: one call that waits for many.
 *
 * Thirty reads are issued, each into a buffer of its own. wait_all turns the
 * thirty buffers into one group pointer without waiting for any of them.
 * write_byte takes that pointer as its last argument, so the write is queued
 * behind all thirty reads: it runs only after the last read has landed, and
 * no read sees its byte.
 *
 * Nothing here waits. pragma_report.hh does the setup, waits for the results
 * at the end and prints what happened. */

#include "pragma_report.hh"

FILC_ASYNC(io_uring, FILC_OP(pread))
void* read_byte(int fd, void* buf, size_t len, long offset) {
  pragma_body_calls++;
  return 0;
}

FILC_ASYNC(io_uring, FILC_OP(pwrite))
void* write_byte(int fd, const void* buf, size_t len, long offset,
                 prov_tag after) {
  pragma_body_calls++;
  return 0;
}

int main(int argc, char** argv) {
  struct waitall w = waitall_setup(argc, argv);
  void* reads[WAITALL_READS];

  for (int i = 0; i < WAITALL_READS; i++)
    reads[i] = read_byte(w.fd, w.byte[i], 1, i);

  prov_tag group = filc_async_wait_all(w.tag, WAITALL_READS);
  void* writer = write_byte(w.fd, "!", 1, 0, group);

  return waitall_report(&w, reads, group, writer);
}
