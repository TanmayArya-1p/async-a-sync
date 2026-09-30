/* probe_io_uring.c -- exits 0 when this process can use io_uring.
 *
 * tests/run.sh uses it to skip, rather than fail, the tests that need a ring.
 * io_uring is commonly missing even on a new enough kernel: Docker's default
 * seccomp profile blocks it, kernel.io_uring_disabled can turn it off, and
 * x86-64 emulators such as Rosetta do not implement it. Plain C on purpose:
 * this is a property of the environment, not of the runtime. */

#include <errno.h>
#include <stddef.h>
#include <sys/syscall.h>
#include <unistd.h>

#ifndef SYS_io_uring_setup
#define SYS_io_uring_setup 425
#endif

int main(void) {
  /* a null params pointer fails with EFAULT once the call is reachable */
  long ret = syscall(SYS_io_uring_setup, 1, NULL);
  if (ret >= 0) {
    close((int)ret);
    return 0;
  }
  return errno == ENOSYS || errno == EPERM ? 1 : 0;
}
