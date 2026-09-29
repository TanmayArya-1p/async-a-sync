/* demo_rpc_upload: read some files and upload them, on two runtimes.
 *
 * async_pread() runs on runtime=io_uring and upload() on runtime=rpc (see
 * rpc_upload.hh). The loop reads each file into a buffer and uploads that
 * buffer, and neither call waits: the read only queues a request, and the
 * upload connects to the server but holds the bytes back while the read is
 * still filling them. Waiting for the first upload sends every queued read
 * to the kernel in one batch; each upload then sends its bytes as soon as
 * its read lands.
 *
 * rpc_upload_report.hh creates the files, prints the server's checksums and
 * checks them against the files. */

#include "rpc_upload_report.hh"

int main(int argc, char** argv) {
  struct upload u = upload_setup(argc, argv);
  void* sent[UPLOAD_FILES];

  for (int i = 0; i < UPLOAD_FILES; i++) {
    async_pread(u.fd[i], u.buf[i], UPLOAD_BYTES, 0);
    sent[i] = upload(u.port, u.buf[i], UPLOAD_BYTES);
  }
  upload_issued(&u);

  return upload_report(&u, sent);
}
