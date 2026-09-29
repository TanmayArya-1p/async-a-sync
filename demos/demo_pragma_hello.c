/* demo_pragma_hello: what one annotated call does.
 *
 * The pragma on read_block tells the patched compiler that the function
 * stands for pread(2), that argument 0 is the fd and that argument 1 is a
 * buffer the kernel fills. The compiler rewrites every call to it:
 *
 *   1. the call queues an io_uring request and returns at once; the body
 *      never runs and buf is marked pending;
 *   2. the first read of buf sends the request to the kernel and waits for
 *      it, through the check the compiler put in front of that load.
 *
 * No wait is written anywhere. pragma_report.hh times each step and prints
 * what the runtime did. */

#include "pragma_report.hh"

#pragma clang attribute push(__attribute__((annotate("filc_async", "op=pread", "fd=0", "bout=1"))), apply_to=function)
void* read_block(int fd, void* buf, size_t len, unsigned long offset);
#pragma clang attribute pop

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

/* Never runs: it exists so the program links. */
void* read_block(int fd, void* buf, size_t len, unsigned long offset) {
  pragma_body_calls++;
  return 0;
}
