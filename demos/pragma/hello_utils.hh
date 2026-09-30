#pragma once

// Setup and report for demo_pragma_hello.

#include "pragma_utils.hh"

static const char hello_text[] =
    "Hello from io_uring: these bytes were read by an annotated call.";

struct demo {
  char path[256];
  int fd;
  char* buf;
  size_t len;
  double t0;
  struct counters start;
};

static inline struct demo setup(int argc, char** argv) {
  struct demo d = {0};
  title("hello: one annotated call",
        "  read_block() is plain C; its pragma says op=pread.");
  d.fd = make_file(d.path, sizeof(d.path), dir_arg(argc, argv),
                   "demo_pragma_hello.txt", hello_text);
  d.len = strlen(hello_text);
  d.buf = (char*)calloc(1, 128);
  warm_up(d.fd);
  posix_fadvise(d.fd, 0, d.len, POSIX_FADV_DONTNEED);
  d.start = counters_now();
  d.t0 = now_ms();
  return d;
}

// Called right after main read buf[0].
static inline int report(struct demo* d, char first) {
  double read_ms = now_ms() - d->t0;
  struct counters used = counters_since(d->start);
  unsigned long entries_at_call =
      calls.n ? calls.call[0].entries - (d->start.submits + d->start.waits) : 0;

  printf("  %-22s %9s %15s\n", "step", "time", "kernel entries");
  printf("  %-22s %6.3f ms %15lu\n", "read_block() called",
         calls.n ? calls.call[0].ms - d->t0 : 0, entries_at_call);
  printf("  %-22s %6.3f ms %15lu\n", "buf[0] read", read_ms,
         used.submits + used.waits);
  printf("\n  buf[0] is '%c'; buf holds:\n  \"%s\"\n", first, d->buf);
  printf("\n  => one call, one request; reading buf sent it and waited for it\n");

  check_bodies();
  check("reading buf found it pending and waited", used.hook_waits == 1);
  check("the first read of buf sent the request", used.submits == 1);
  check("buf holds the file's bytes",
        first == hello_text[0] && equal(d->buf, hello_text, d->len));

  free(d->buf);
  close(d->fd);
  unlink(d->path);
  return finish();
}
