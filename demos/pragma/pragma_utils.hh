#pragma once

// What the pragma demos share: the call log, counters and timing. Each
// demo's own setup and report are in <demo>_utils.hh, so each demo file
// holds only its annotated functions and their calls.

#include <errno.h>

#include "fasync.h"
#include "filc_async.h"
#include "../common/utils.hh"

#ifndef FASYNC_COMPILER_INSERTS_CHECKS
#error "the pragma demos need the patched compiler: \
build with -DFASYNC_COMPILER_INSERTS_CHECKS"
#endif

// ---- the call log ----
// Each annotated function's body calls log_call. The io_uring runtime runs
// a body just before it queues the call's request.

#define LOG_MAX 64

struct logged_call {
  const char* name;
  double ms;             // when the body ran
  unsigned long entries; // kernel entries made before it ran
};

static struct {
  int n;          // bodies run
  double last_ms; // when the last one ran
  struct logged_call call[LOG_MAX];
} calls __attribute__((unused));

static inline unsigned long kernel_entries(void) {
  struct fasync_stats s;
  fasync_get_stats(&s);
  return s.kernel_submit_entries + s.kernel_wait_entries;
}

static inline void log_call(const char* name) {
  double ms = now_ms();
  if (calls.n < LOG_MAX)
    calls.call[calls.n] = (struct logged_call){name, ms, kernel_entries()};
  calls.n++;
  calls.last_ms = ms;
}

// Waits for an annotated call; returns its result (-errno on failure).
static inline long wait_for(void* task) {
  struct filc_async_result_s r = {0};
  r.pending = task;
  filc_async_wait(&r);
  return r.result;
}

// ---- counters ----

// What the runtime and the kernel did.
struct counters {
  unsigned long calls;     // annotated calls made
  unsigned long completed; // ... and completed
  unsigned long failed;    // ... with -errno
  unsigned long sqes;      // io_uring requests queued
  unsigned long submits;   // io_uring_enter calls that submitted
  unsigned long waits;     // io_uring_enter calls that slept
  unsigned long hook_waits; // accesses that found their buffer pending
  unsigned long sleeps;    // times this thread blocked
};

static inline struct counters counters_now(void) {
  filc_async_stats a;
  struct fasync_stats f;
  struct rusage ru;
  filc_async_get_stats(&a);
  fasync_get_stats(&f);
  getrusage(RUSAGE_THREAD, &ru);
  return (struct counters){a.tasks_submitted, a.tasks_completed,
                           a.tasks_failed, f.sqes_queued,
                           f.kernel_submit_entries, f.kernel_wait_entries,
                           a.hook_resolves, (unsigned long)ru.ru_nvcsw};
}

static inline struct counters counters_since(struct counters then) {
  struct counters now = counters_now();
  now.calls -= then.calls;
  now.completed -= then.completed;
  now.failed -= then.failed;
  now.sqes -= then.sqes;
  now.submits -= then.submits;
  now.waits -= then.waits;
  now.hook_waits -= then.hook_waits;
  now.sleeps -= then.sleeps;
  return now;
}

// The io_uring runtime runs the body of every annotated call once.
static inline void check_bodies(void) {
  check("the runtime ran each annotated call's body once",
        (unsigned long)calls.n == counters_now().calls);
}

// The first request sets up the io_uring ring. Pays for that untimed.
static inline void warm_up(int fd) {
  fasync_result(fasync_fsync(fd));
}

static inline const char* dir_arg(int argc, char** argv) {
  return argc > 1 ? argv[1] : "/tmp";
}

// Makes a file holding text; returns an fd for reading and writing.
static inline int make_file(char* path, size_t size, const char* dir,
                            const char* name, const char* text) {
  snprintf(path, size, "%s/%s", dir, name);
  int fd = open(path, O_CREAT | O_TRUNC | O_RDWR, 0644);
  size_t len = strlen(text);
  if (fd < 0 || pwrite(fd, text, len, 0) != (ssize_t)len || fsync(fd) != 0) {
    perror(path);
    exit(1);
  }
  return fd;
}

// ---- timing ----

#define READ_BYTES 4096
#define MAX_PASSES 15

typedef size_t (*read_fn)(int n);

struct pass {
  double ms;
  double issue_ms; // until the last annotated call ran
  size_t words;
  struct counters used;
};

// Reads every file cold and then cached. Without a difference the async
// reads have nothing to overlap, and the speedups mean nothing.
static inline int cache_matters(void) {
  double t, cold, warm;
  drop_cache();
  t = now_ms();
  for (int i = 0; i < files.n; i++)
    pread(files.fd[i], files.buf[i], files.bytes, 0);
  cold = now_ms() - t;
  t = now_ms();
  for (int i = 0; i < files.n; i++)
    pread(files.fd[i], files.buf[i], files.bytes, 0);
  warm = now_ms() - t;

  double ratio = cold / (warm > 0 ? warm : 1e-9);
  if (ratio <= 1.5) {
    printf("  !! dropping the page cache made no difference here (%.2f vs %.2f ms),\n"
           "     so there is no device latency to overlap and the speedups below\n"
           "     mean nothing.\n\n",
           cold, warm);
    return 0;
  }
  printf("  page cache dropped: cold reads are %.0fx slower than cached ones\n\n",
         ratio);
  return 1;
}

// Reads [files] [passes] from the command line, after [dir].
static inline void read_args(int argc, char** argv, int* files_n, int* passes) {
  if (argc > 2)
    *files_n = atoi(argv[2]);
  if (argc > 3)
    *passes = atoi(argv[3]);
  if (*files_n < 1 || *files_n > MAX_FILES || *passes < 1 || *passes > MAX_PASSES) {
    fprintf(stderr, "usage: %s [dir] [files 1..%d] [passes 1..%d]\n", argv[0],
            MAX_FILES, MAX_PASSES);
    exit(2);
  }
}

// Makes n files of `bytes`, and warms up the ring. Returns whether dropping
// the page cache makes reads slower.
static inline int prepare_files(int argc, char** argv, int n, size_t bytes) {
  make_files(dir_arg(argc, argv), n, bytes);
  int matters = cache_matters();
  warm_up(files.fd[0]);
  return matters;
}

// Times one pass of `read` over the first n files, from empty buffers and a
// dropped page cache.
static inline void time_pass(struct pass* p, read_fn read, int n) {
  memset(p, 0, sizeof(*p));
  for (int i = 0; i < n; i++)
    memset(files.buf[i], 0, files.bytes);
  drop_cache();
  struct counters start = counters_now();
  double t = now_ms();
  p->words = read(n);
  p->ms = now_ms() - t;
  p->issue_ms = calls.last_ms > t ? calls.last_ms - t : 0;
  p->used = counters_since(start);
}

enum pass_field { MS, ISSUE_MS, ENTRIES, SLEEPS };

static inline double pass_value(const struct pass* p, enum pass_field f) {
  switch (f) {
  case MS:
    return p->ms;
  case ISSUE_MS:
    return p->issue_ms;
  case ENTRIES:
    return (double)(p->used.submits + p->used.waits);
  case SLEEPS:
    return (double)p->used.sleeps;
  }
  return 0;
}

// The median of one field across a way's passes.
static inline double pass_median(const struct pass* passes, int n,
                                 enum pass_field f) {
  double v[MAX_PASSES];
  for (int i = 0; i < n; i++)
    v[i] = pass_value(&passes[i], f);
  return median(v, n);
}

static inline int words_counted(const struct pass* passes, int n, int nfiles) {
  int ok = 1;
  for (int i = 0; i < n; i++)
    ok &= passes[i].words == words_in_files(nfiles);
  return ok;
}

static inline double ratio(double a, double b) {
  return a / (b > 0 ? b : 1e-9);
}
