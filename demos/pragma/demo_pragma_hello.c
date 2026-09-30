/* hello: one annotated call.
 *
 * read_block stands for pread. Calling it queues the read and returns at
 * once. The first read of buf waits for the data. No wait is written. */

#include "pragma_utils.hh"

// pread: the kernel fills buf.
#pragma clang attribute push(__attribute__((annotate("filc_async", "runtime=io_uring", "op=pread", "bout=buf"))), apply_to=function)
void* read_block(int fd, void* buf, size_t len, unsigned long offset) {
  log_call("read_block"); // runs as the call is made
  return 0;
}
#pragma clang attribute pop

int main(int argc, char** argv) {
  struct hello h = hello_setup(argc, argv);

  read_block(h.fd, h.buf, h.len, 0); // queues the read, returns at once
  char first = h.buf[0];             // waits here for the read

  return hello_report(&h, first);
}
