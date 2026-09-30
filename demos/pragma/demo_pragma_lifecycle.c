/* lifecycle: open, write, fsync, read back, close.
 *
 * Each call becomes an io_uring request. They are issued back to back with
 * no wait between them. Each names the fd in w_dep= or r_dep=, so each one
 * runs after the call before it on that fd. */

#include "lifecycle_utils.hh"

#pragma clang attribute push(__attribute__((annotate("filc_async", "runtime=io_uring", "op=openat", "bin=path"))), apply_to=function)
void* async_openat(int dirfd, const char* path, int flags, int mode) {
  log_call("async_openat");
  return 0;
}
#pragma clang attribute pop

#pragma clang attribute push(__attribute__((annotate("filc_async", "runtime=io_uring", "op=pwrite", "bin=buf", "w_dep=fd:file"))), apply_to=function)
void* async_pwrite(int fd, const void* buf, size_t len, unsigned long offset) {
  log_call("async_pwrite");
  return 0;
}
#pragma clang attribute pop

#pragma clang attribute push(__attribute__((annotate("filc_async", "runtime=io_uring", "op=fsync", "w_dep=fd:file"))), apply_to=function)
void* async_fsync(int fd) {
  log_call("async_fsync");
  return 0;
}
#pragma clang attribute pop

#pragma clang attribute push(__attribute__((annotate("filc_async", "runtime=io_uring", "op=pread", "bout=buf", "r_dep=fd:file"))), apply_to=function)
void* async_pread(int fd, void* buf, size_t len, unsigned long offset) {
  log_call("async_pread");
  return 0;
}
#pragma clang attribute pop

#pragma clang attribute push(__attribute__((annotate("filc_async", "runtime=io_uring", "op=close", "w_dep=fd:file"))), apply_to=function)
void* async_close(int fd) {
  log_call("async_close");
  return 0;
}
#pragma clang attribute pop

static const char text[] = "Written, flushed and read back by io_uring.";

int main(int argc, char** argv) {
  struct demo d = setup(argc, argv, text);
  size_t len = sizeof(text) - 1;
  char back[64] = {0};

  // The other calls need the fd, so wait for this one.
  int fd = wait_for(async_openat(AT_FDCWD, d.path, O_CREAT | O_RDWR, 0644));

  void* wrote = async_pwrite(fd, text, len, 0);
  void* synced = async_fsync(fd);                  // after the write
  void* read_back = async_pread(fd, back, len, 0); // after the fsync
  void* closed = async_close(fd);                  // after the read

  return report(&d, fd, back, wrote, synced, read_back, closed);
}
