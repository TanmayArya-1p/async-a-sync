#pragma once

/* Setup, measurement, tables and checks for the demo_pragma_* programs, one
 * section per demo, so each demo's source holds only the code it
 * demonstrates. Nothing here changes what a demo does: it creates the files,
 * snapshots the runtime's counters around the demonstrated code, prints the
 * results and checks them. */

#include <errno.h>

#include "pragma_io.hh"

#define PRAGMA_PATH_MAX 256

static inline const char* pragma_dir(int argc, char** argv) {
  return argc > 1 ? argv[1] : "/tmp";
}

/* ---- demo_pragma_hello ---- */

struct hello_step {
  const char* what;
  double us;
  struct pragma_snap used; /* since the first step started */
  int pending;             /* whether buf was pending after the step */
};

struct hello {
  char path[PRAGMA_PATH_MAX];
  int fd;
  char* buf;
  size_t len;
  struct pragma_snap start;
  double t0;
  struct hello_step step[2];
  int nsteps;
};

static const char hello_message[] =
    "Hello from io_uring: these bytes were read by an annotated call.";

/* Writes the message to a file and drops the file from the page cache, so
 * the demo's read is a device round trip. */
static inline struct hello hello_setup(int argc, char** argv) {
  struct hello h = {0};
  h.len = sizeof(hello_message) - 1;
  pragma_title("hello: one annotated call, step by step",
               "  read_block() is plain C; its pragma says op=pread.");

  snprintf(h.path, sizeof(h.path), "%s/demo_pragma_hello.txt",
           pragma_dir(argc, argv));
  h.fd = open(h.path, O_CREAT | O_TRUNC | O_RDWR, 0644);
  if (h.fd < 0 || pwrite(h.fd, hello_message, h.len, 0) != (ssize_t)h.len ||
      fsync(h.fd) != 0) {
    perror(h.path);
    exit(1);
  }
  h.buf = (char*)calloc(1, 128);

  /* The first request sets up the io_uring ring. Pay for that untimed. */
  pragma_wait(async_pread(h.fd, h.buf, 1, 0));
  h.buf[0] = 0;
  posix_fadvise(h.fd, 0, h.len, POSIX_FADV_DONTNEED);
  pragma_snap(&h.start);
  return h;
}

static inline void step_start(struct hello* h) {
  h->t0 = demo_now_ms();
}

static inline void step_done(struct hello* h, const char* what) {
  struct hello_step* s = &h->step[h->nsteps++];
  s->us = (demo_now_ms() - h->t0) * 1000.0;
  s->what = what;
  s->used = pragma_since(&h->start);
  s->pending = filc_async_is_pending(h->buf);
}

static inline int hello_report(struct hello* h, char first, long n) {
  struct hello_step* call = &h->step[0];
  struct hello_step* touch = &h->step[1];

  printf("  %-26s %9s %15s   %s\n", "step", "time", "kernel entries",
         "buf pending");
  for (int i = 0; i < h->nsteps; i++)
    printf("  %-26s %6.1f us %15lu   %s\n", h->step[i].what, h->step[i].us,
           h->step[i].used.submits + h->step[i].used.waits,
           h->step[i].pending ? "yes" : "no");
  printf("\n  body of read_block ran %d times; buf[0] is '%c'; buf holds:\n"
         "  \"%s\"\n",
         pragma_body_calls, first, h->buf);
  printf("\n  => one call, one request; reading buf sent it (%lu kernel entry), "
         "no wait written\n",
         touch->used.submits + touch->used.waits);

  pragma_check("the body of read_block never ran", pragma_body_calls == 0);
  pragma_check("the call only queued a request (no kernel entry)",
               call->used.sqes == 1 && call->used.submits == 0 &&
                   call->used.waits == 0);
  pragma_check("buf was pending until it was first read",
               call->pending && !touch->pending);
  pragma_check("the first read sent the request to the kernel",
               touch->used.submits == 1);
  pragma_check("the bytes are the ones in the file",
               n == (long)h->len && memcmp(h->buf, hello_message, h->len) == 0);

  free(h->buf);
  close(h->fd);
  unlink(h->path);
  return pragma_finish();
}

/* ---- demo_pragma_lifecycle ---- */

struct lifecycle {
  char path[PRAGMA_PATH_MAX];
  struct pragma_snap start;
};

struct lifecycle_results {
  int fd;
  long wrote, synced, read, closed;
};

static inline struct lifecycle lifecycle_setup(int argc, char** argv) {
  struct lifecycle l = {0};
  pragma_title("lifecycle: open, write, fsync, read, close",
               "  Five annotated calls, issued with no wait between them.");
  snprintf(l.path, sizeof(l.path), "%s/demo_pragma_lifecycle.txt",
           pragma_dir(argc, argv));
  unlink(l.path);
  pragma_snap(&l.start);
  demo_start();
  return l;
}

static inline int lifecycle_report(struct lifecycle* l,
                                   const struct lifecycle_results* r,
                                   const char* text, const char* back) {
  double ms = demo_elapsed();
  struct pragma_snap used = pragma_since(&l->start);
  size_t len = strlen(text);

  printf("  %-24s %s\n", "call", "result");
  printf("  %-24s fd %d\n", "async_openat(path)", r->fd);
  printf("  %-24s %ld bytes written\n", "async_pwrite(fd, text)", r->wrote);
  printf("  %-24s %ld\n", "async_fsync(fd)", r->synced);
  printf("  %-24s %ld bytes: \"%s\"\n", "async_pread(fd, back)", r->read, back);
  printf("  %-24s %ld\n", "async_close(fd)", r->closed);
  printf("\n  %lu calls, %lu io_uring requests, %d bodies run, %lu kernel "
         "entries, %.2f ms\n",
         used.calls, used.sqes, pragma_body_calls, used.submits + used.waits, ms);
  printf("\n  => 5 annotated syscalls ran in program order, no wait between "
         "them\n");

  /* Check the file with ordinary blocking calls. */
  char disk[128] = {0};
  int check_fd = open(l->path, O_RDONLY);
  ssize_t disk_n = check_fd >= 0 ? read(check_fd, disk, sizeof(disk)) : -1;
  if (check_fd >= 0)
    close(check_fd);
  errno = 0;
  int fd_closed = fcntl(r->fd, F_GETFD) == -1 && errno == EBADF;

  pragma_check("every step succeeded",
               r->fd >= 0 && r->wrote == (long)len && r->synced == 0 &&
                   r->read == (long)len && r->closed == 0);
  pragma_check("the read saw the write issued before it",
               memcmp(back, text, len) == 0);
  pragma_check("the file holds the text (checked with plain read)",
               disk_n == (ssize_t)len && memcmp(disk, text, len) == 0);
  pragma_check("the fd was closed by the last call", fd_closed);
  pragma_check("five calls, five requests, no bodies run",
               used.calls == 5 && used.sqes == 5 && pragma_body_calls == 0);

  unlink(l->path);
  return pragma_finish();
}

/* ---- demo_pragma_ordering ---- */

#define ORDERING_FILES 8
#define ORDERING_RECORD 16

/* What a scenario's calls should do while they are being issued. */
enum { INDEPENDENT, CONFLICTING };

struct scenario {
  const char* what;
  int expect;
  unsigned long calls;
  unsigned long issuing; /* kernel entries made inside the calls */
  unsigned long submits; /* io_uring_enter calls that submitted, in total */
  int ok;
  const char* outcome;
};

static char ordering_paths[ORDERING_FILES][PRAGMA_PATH_MAX];
static struct scenario scenarios[4];
static int nscenarios;
static struct pragma_snap scenario_snap;
static struct pragma_snap scenario_issued_snap;

/* Opens ORDERING_FILES scratch files into fd[]. */
static inline void ordering_setup(int argc, char** argv, int* fd) {
  pragma_title("ordering: dependencies order calls, the rest batch",
               "  Calls that conflict on an fd wait; independent calls batch.");
  for (int i = 0; i < ORDERING_FILES; i++) {
    snprintf(ordering_paths[i], PRAGMA_PATH_MAX, "%s/demo_pragma_ordering_%d.dat",
             pragma_dir(argc, argv), i);
    fd[i] = open(ordering_paths[i], O_CREAT | O_TRUNC | O_RDWR, 0644);
    if (fd[i] < 0) {
      perror(ordering_paths[i]);
      exit(1);
    }
  }
  /* The first request sets up the io_uring ring; keep that out of the rows. */
  pragma_wait(async_fsync(fd[0]));
}

static inline void scenario_start(const char* what, int expect) {
  struct scenario* s = &scenarios[nscenarios];
  s->what = what;
  s->expect = expect;
  pragma_snap(&scenario_snap);
}

static inline void scenario_issued(void) {
  scenario_issued_snap = pragma_since(&scenario_snap);
}

static inline void scenario_done(int ok, const char* outcome) {
  struct scenario* s = &scenarios[nscenarios++];
  struct pragma_snap done = pragma_since(&scenario_snap);
  s->calls = scenario_issued_snap.calls;
  s->issuing = scenario_issued_snap.submits + scenario_issued_snap.waits;
  s->submits = done.submits;
  s->ok = ok;
  s->outcome = ok ? outcome : "FAILED";

  pragma_check(s->what, ok);
  if (s->expect == CONFLICTING) {
    pragma_check("  conflicting calls waited inside the call", s->issuing > 0);
  } else {
    pragma_check("  independent calls did not wait while issued", s->issuing == 0);
    pragma_check("  the batch reached the kernel in one submit", s->submits == 1);
  }
}

/* Fills the first file with numbered records for the reads scenario. */
static inline void write_records(int fd) {
  char record[ORDERING_RECORD + 1];
  for (int i = 0; i < ORDERING_FILES; i++) {
    snprintf(record, sizeof(record), "record number %02d", i);
    if (pwrite(fd, record, ORDERING_RECORD, (off_t)i * ORDERING_RECORD) !=
        ORDERING_RECORD) {
      perror("write_records");
      exit(1);
    }
  }
}

/* Compares byte by byte in compiled code, so the compiler's access check
 * resolves a pending buffer (a libc memcmp would not). */
static inline int holds_record(const char* buf, int i) {
  char record[ORDERING_RECORD + 1];
  snprintf(record, sizeof(record), "record number %02d", i);
  int ok = 1;
  for (int j = 0; j < ORDERING_RECORD; j++)
    ok &= buf[j] == record[j];
  return ok;
}

static inline int ordering_report(int* fd) {
  /* waited: a call entered the kernel before returning, to wait for the
   * conflicting call before it. submits: io_uring_enter calls that submitted,
   * waits included. */
  printf("  %-30s %5s %7s %8s   %s\n", "scenario", "calls", "waited",
         "submits", "outcome");
  for (int i = 0; i < nscenarios; i++)
    printf("  %-30s %5lu %7s %8lu   %s\n", scenarios[i].what, scenarios[i].calls,
           scenarios[i].issuing ? "yes" : "no", scenarios[i].submits,
           scenarios[i].outcome);
  printf("\n  => conflicting calls kept their order; %lu independent calls, "
         "%lu kernel submit\n",
         scenarios[1].calls, scenarios[1].submits);

  for (int i = 0; i < ORDERING_FILES; i++) {
    close(fd[i]);
    unlink(ordering_paths[i]);
  }
  pragma_check("no annotated body ran", pragma_body_calls == 0);
  return pragma_finish();
}
