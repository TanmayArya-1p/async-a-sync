/* upload: read some files and upload them, on two runtimes.
 *
 * async_pread runs on runtime=io_uring; upload runs on runtime=rpc. Neither
 * call waits. The upload holds its bytes back until the read filling them
 * lands. Waiting for the first upload sends all the reads to the kernel in
 * one batch.
 *
 * Logging, setup and checks: rpc_utils.hh. */

#include "rpc_utils.hh"

// pread on io_uring: the kernel fills buf.
#pragma clang attribute push(__attribute__((annotate("filc_async", "runtime=io_uring", "op=pread", "bout=buf"))), apply_to=function)
void* async_pread(int fd, void* buf, size_t len, unsigned long offset) {
  log_call("async_pread");
  return 0;
}
#pragma clang attribute pop

// put on rpc: sends len bytes of data; the reply is their checksum.
#pragma clang attribute push(__attribute__((annotate("filc_async", "runtime=rpc", "op=put", "bin=data"))), apply_to=function)
void* upload(unsigned port, const void* data, size_t len) {
  log_call("upload");
  return 0;
}
#pragma clang attribute pop

int main(int argc, char** argv) {
  struct upload u = upload_setup(argc, argv);
  void* sent[UPLOAD_FILES];

  for (int i = 0; i < UPLOAD_FILES; i++) {
    async_pread(u.fd[i], u.buf[i], UPLOAD_BYTES, 0);  // queues the read
    sent[i] = upload(u.port, u.buf[i], UPLOAD_BYTES); // sends once the read lands
  }

  return upload_report(&u, sent);
}
