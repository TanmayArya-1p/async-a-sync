/* demo_pragma_hello: what one annotated call does.
 *
 * The annotation on read_block tells the patched compiler that the function
 * stands for pread(2); argument 0 is the fd, and buf, a pointer to non-const,
 * is a buffer the kernel fills. The compiler rewrites every call to it:
 *
 *   1. the call marks buf pending and hands the call to the runtime, which
 *      runs the body and queues an io_uring request; the call returns at
 *      once;
 *   2. the first read of buf sends the request to the kernel and waits for
 *      it, through the check the compiler put in front of that load.
 *
 * No wait is written anywhere. pragma_report.hh times each step and prints
 * what the runtime did. */

#include "pragma_report.hh"

FILC_ASYNC(io_uring, FILC_OP(pread))
void* read_block(int fd, void* buf, size_t len, long offset);

int main(int argc, char** argv) {
  struct hello h = hello_setup(argc, argv);
  char* buf = h.buf;

  step_start(&h);
  void* task = read_block(h.fd, buf, h.len, 0);
  step_done(&h, "call read_block()");

  step_start(&h);
  char first = buf[0];
  step_done(&h, "first read: buf[0]");

  /* The handle is still there for code that wants the result. */
  long n = pragma_wait(task);
  return hello_report(&h, first, n);
}

/* The runtime runs this before it issues the read; a program could log or
 * instrument its calls here. */
void* read_block(int fd, void* buf, size_t len, long offset) {
  pragma_body_calls++;
  return 0;
}
