/* demo_pragma_lifecycle: a whole file lifecycle through annotated calls.
 *
 * open, write, fsync, read back, close. Each is a call to one of the
 * annotated functions in pragma_io.hh, so each becomes an io_uring request.
 * After the open, the four calls are issued back to back with no wait between
 * them. They all name the fd in a w_dep= or r_dep= option, so the runtime
 * starts each one only after the call before it on that fd has finished: the
 * code reads like blocking code and runs in the order it is written. */

#include "pragma_report.hh"

const char text[] = "Written, flushed and read back by io_uring.";

int main(int argc, char** argv) {
  struct lifecycle l = lifecycle_setup(argc, argv);
  size_t len = sizeof(text) - 1;
  char back[64] = {0};

  /* The next calls need the fd, so this one is waited on. */
  int fd = pragma_wait(
      async_openat(AT_FDCWD, l.path, O_CREAT | O_TRUNC | O_RDWR, 0644));

  void* wrote = async_pwrite(fd, text, len, 0);
  void* synced = async_fsync(fd);
  void* read_back = async_pread(fd, back, len, 0);
  void* closed = async_close(fd);

  struct lifecycle_results r = {
      .fd = fd,
      .wrote = pragma_wait(wrote),
      .synced = pragma_wait(synced),
      .read = pragma_wait(read_back),
      .closed = pragma_wait(closed),
  };
  return lifecycle_report(&l, &r, text, back);
}
