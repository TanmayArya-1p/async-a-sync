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
  printf("\n  the runtime ran read_block's body before the read; buf[0] is "
         "'%c'; buf holds:\n  \"%s\"\n",
         first, h->buf);
  printf("\n  => one call, one request; reading buf sent it (%lu kernel entry), "
         "no wait written\n",
         touch->used.submits + touch->used.waits);

  pragma_check_bodies();
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
  pragma_check("five calls, five requests, five bodies run",
               used.calls == 5 && used.sqes == 5 && pragma_body_calls == 5);

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
  pragma_check_bodies();
  return pragma_finish();
}

/* ---- demo_pragma_coldread and demo_pragma_scaling ---- */

#define READS_FILE_BYTES 4096
#define PRAGMA_MAX_PASSES 15

typedef size_t (*read_fn)(int n);

struct pass {
  double ms;
  double issue_ms; /* read_annotated: time spent inside the calls */
  size_t words;
  struct pragma_snap used;
};

struct timing {
  int files; /* scaling: the most files */
  int passes;
  int regime_ok;
  struct pass blocking[PRAGMA_MAX_PASSES];
  struct pass annotated[PRAGMA_MAX_PASSES];
  struct pass by_hand[PRAGMA_MAX_PASSES];

  /* scaling's running results */
  int words_ok;
  int batched;
  double best;
  int best_n;
  double last_call_us;
  double last_speedup;
  int last_n;
};

/* Parses [dir] [files] [passes]. */
static inline struct timing timing_args(int argc, char** argv, int files,
                                        int passes) {
  struct timing t = {0};
  t.files = argc > 2 ? atoi(argv[2]) : files;
  t.passes = argc > 3 ? atoi(argv[3]) : passes;
  t.words_ok = 1;
  t.batched = 1;
  if (t.files < 1 || t.files > DEMO_MAX_FILES || t.passes < 1 ||
      t.passes > PRAGMA_MAX_PASSES) {
    fprintf(stderr, "usage: %s [dir] [files 1..%d] [passes 1..%d]\n", argv[0],
            DEMO_MAX_FILES, PRAGMA_MAX_PASSES);
    exit(2);
  }
  return t;
}

/* Creates the files and checks that dropping the page cache adds device
 * latency. */
static inline void timing_prepare(struct timing* t, int argc, char** argv) {
  pragma_raise_fd_limit();
  if (demo_files(pragma_dir(argc, argv), t->files, READS_FILE_BYTES) < 0)
    exit(1);
  t->regime_ok = pragma_regime();

  /* The first request sets up the io_uring ring; keep that out of the passes. */
  pragma_wait(async_pread(demo_fd[0], demo_buf[0], demo_bytes, 0));
}

/* Times one pass of `read` over the first n files, from empty buffers and a
 * dropped page cache. */
static inline void time_pass(struct pass* p, read_fn read, int n) {
  memset(p, 0, sizeof(*p));
  for (int i = 0; i < n; i++)
    memset(demo_buf[i], 0, demo_bytes);
  demo_cold();
  struct pragma_snap start;
  pragma_snap(&start);
  pragma_issue_ms = 0;
  demo_start();
  p->words = read(n);
  p->ms = demo_elapsed();
  p->issue_ms = pragma_issue_ms;
  p->used = pragma_since(&start);
}

enum pass_field { F_MS, F_ISSUE_MS, F_ENTRIES, F_SLEEPS };

static inline double pass_value(const struct pass* p, enum pass_field f) {
  switch (f) {
  case F_MS:
    return p->ms;
  case F_ISSUE_MS:
    return p->issue_ms;
  case F_ENTRIES:
    return (double)(p->used.submits + p->used.waits);
  case F_SLEEPS:
    return (double)p->used.sleeps;
  }
  return 0;
}

/* The median of one field across a way's passes. */
static inline double pass_median(const struct pass* passes, int n,
                                 enum pass_field f) {
  double v[PRAGMA_MAX_PASSES];
  for (int i = 0; i < n; i++)
    v[i] = pass_value(&passes[i], f);
  return pragma_median(v, n);
}

static inline int words_counted(const struct pass* passes, int n, int files) {
  size_t expect = 0;
  for (int i = 0; i < files; i++)
    expect += demo_expect[i];
  int ok = 1;
  for (int i = 0; i < n; i++)
    ok &= passes[i].words == expect;
  return ok;
}

static inline double ratio(double a, double b) {
  return a / (b > 0 ? b : 1e-9);
}

static inline struct timing coldread_setup(int argc, char** argv) {
  pragma_title("coldread: the same loop, blocking and annotated", 0);
  struct timing t = timing_args(argc, argv, 512, 5);
  printf("  Word count over %d cold files of %d bytes, median of %d passes.\n"
         "  B is A's loop with pread renamed to async_pread.\n\n",
         t.files, READS_FILE_BYTES, t.passes);
  timing_prepare(&t, argc, argv);
  return t;
}

static inline int coldread_report(struct timing* t) {
  static const char* const name[] = {"A blocking pread",
                                     "B annotated async_pread",
                                     "C hand-written fasync_*"};
  const struct pass* way[] = {t->blocking, t->annotated, t->by_hand};
  double ms[3];
  for (int w = 0; w < 3; w++)
    ms[w] = pass_median(way[w], t->passes, F_MS);

  /* kernel entries: one per pread for A; io_uring_enter submits + waits for
   * B and C. sleeps: times this thread blocked (voluntary context switches
   * from getrusage). */
  printf("  %-26s %8s %7s %15s %7s\n", "arm", "ms", "vs A", "kernel entries",
         "sleeps");
  for (int w = 0; w < 3; w++) {
    double entries =
        w == 0 ? t->files : pass_median(way[w], t->passes, F_ENTRIES);
    printf("  %-26s %8.2f %6.2fx %15.0f %7.0f\n", name[w], ms[w],
           ratio(ms[0], ms[w]), entries,
           pass_median(way[w], t->passes, F_SLEEPS));
  }
  printf("\n  => %.2fx faster than blocking pread over %d cold files, same "
         "loop\n",
         ratio(ms[0], ms[1]), t->files);

  int words_ok = 1;
  for (int w = 0; w < 3; w++)
    words_ok &= words_counted(way[w], t->passes, t->files);
  pragma_check("every pass of every arm counted every word", words_ok);
  pragma_check_bodies();
  pragma_check("B entered the kernel far less than once per file",
               pass_median(t->annotated, t->passes, F_ENTRIES) <
                   t->files / 4.0 + 1);
  if (t->regime_ok)
    pragma_check("B beat A on an uncached device", ms[1] < ms[0]);
  else
    printf("  (B vs A not checked: dropping the cache added no device latency)\n");

  demo_finish();
  return pragma_finish();
}

static inline struct timing scaling_setup(int argc, char** argv) {
  pragma_title("scaling: the gain as the number of reads grows", 0);
  struct timing t = timing_args(argc, argv, 2048, 3);
  printf("  Word count over N cold files of %d bytes, N = 1..%d, median of %d\n"
         "  passes. us/call is the time inside each annotated call.\n\n",
         READS_FILE_BYTES, t.files, t.passes);
  timing_prepare(&t, argc, argv);
  printf("  %6s %12s %12s %12s %8s %8s\n", "files", "A pread ms",
         "B async ms", "C by hand ms", "B vs A", "us/call");
  return t;
}

/* Prints the row for n files from the passes just run. */
static inline void scaling_row(struct timing* t, int n) {
  double a = pass_median(t->blocking, t->passes, F_MS);
  double b = pass_median(t->annotated, t->passes, F_MS);
  double c = pass_median(t->by_hand, t->passes, F_MS);
  double call_us = pass_median(t->annotated, t->passes, F_ISSUE_MS) * 1000.0 / n;
  double speedup = ratio(a, b);
  printf("  %6d %12.2f %12.2f %12.2f %7.2fx %8.1f\n", n, a, b, c, speedup,
         call_us);

  t->words_ok &= words_counted(t->blocking, t->passes, n) &&
                 words_counted(t->annotated, t->passes, n) &&
                 words_counted(t->by_hand, t->passes, n);
  /* Below 64 files a pass takes well under a millisecond and the ratio is
   * mostly noise, so the headline's best size is chosen from 64 up. */
  if (n >= 64) {
    t->batched &= pass_median(t->annotated, t->passes, F_ENTRIES) <= n / 4.0 + 2;
    if (speedup > t->best) {
      t->best = speedup;
      t->best_n = n;
    }
  }
  t->last_call_us = call_us;
  t->last_speedup = speedup;
  t->last_n = n;
}

static inline int scaling_report(struct timing* t) {
  /* Above 1024 files the request table is full, so us/call includes waiting
   * for room in it. */
  if (t->best_n)
    printf("\n  => %.2fx faster than blocking at %d files, %.2fx at %d (%.1f us "
           "per call)\n",
           t->best, t->best_n, t->last_speedup, t->last_n, t->last_call_us);

  pragma_check("every pass counted every word", t->words_ok);
  pragma_check_bodies();
  pragma_check("from 64 files up, B batched its reads into few entries",
               t->batched);
  if (t->regime_ok && t->best_n)
    pragma_check("B beat A at some size from 64 up on an uncached device",
                 t->best > 1.0);
  else
    printf("  (B vs A not checked: dropping the cache added no device latency)\n");

  demo_finish();
  return pragma_finish();
}

/* ---- demo_pragma_overlap ---- */

#define OVERLAP_FILE_BYTES 4096

/* How many times the demo's hash mixes each 8-byte word; overlap_calibrate
 * sizes it so hashing every file takes as long as reading them with blocking
 * pread. */
static int hash_rounds = 1;

typedef long (*overlap_fn)(int n);

enum {
  READS_ONLY,
  ASYNC_READS_ONLY,
  HASH_ONLY,
  READ_THEN_HASH,
  ASYNC_THEN_HASH,
  N_OVERLAP_RUNS
};

static const char* const overlap_name[N_OVERLAP_RUNS] = {
    "reads only, pread",
    "reads only, async_pread",
    "hashing only (data in memory)",
    "A  pread + hash, file by file",
    "B  async_pread all, then hash",
};

struct overlap_run {
  double ms;
  long hash;
  unsigned long waits; /* io_uring_enter calls that slept */
};

struct overlap {
  int files;
  int passes;
  int fixed_rounds;
  int regime_ok;
  long expect;
  struct overlap_run run[N_OVERLAP_RUNS][PRAGMA_MAX_PASSES];
};

/* Parses [dir] [files] [passes] [rounds] and creates the files. */
static inline struct overlap overlap_setup(int argc, char** argv) {
  struct overlap o = {0};
  o.files = argc > 2 ? atoi(argv[2]) : 256;
  o.passes = argc > 3 ? atoi(argv[3]) : 5;
  o.fixed_rounds = argc > 4 ? atoi(argv[4]) : 0;
  if (o.files < 1 || o.files > DEMO_MAX_FILES || o.passes < 1 ||
      o.passes > PRAGMA_MAX_PASSES || o.fixed_rounds < 0) {
    fprintf(stderr, "usage: %s [dir] [files 1..%d] [passes 1..%d] [rounds]\n",
            argv[0], DEMO_MAX_FILES, PRAGMA_MAX_PASSES);
    exit(2);
  }

  pragma_title("overlap: the reads run while the program hashes", 0);
  printf("  Read %d cold files of %d bytes and hash each one, median of %d\n"
         "  passes. The hash is sized to take as long as the blocking reads.\n\n",
         o.files, OVERLAP_FILE_BYTES, o.passes);

  pragma_raise_fd_limit();
  if (demo_files(pragma_dir(argc, argv), o.files, OVERLAP_FILE_BYTES) < 0)
    exit(1);
  o.regime_ok = pragma_regime();

  /* The first request sets up the io_uring ring; keep that out of the runs. */
  pragma_wait(async_pread(demo_fd[0], demo_buf[0], demo_bytes, 0));
  return o;
}

/* Times one run. HASH_ONLY needs the data in memory, so it reads it first,
 * untimed; every other run starts from empty buffers and a dropped cache. */
static inline struct overlap_run overlap_time(struct overlap* o, int which,
                                              overlap_fn fn) {
  if (which == HASH_ONLY) {
    for (int i = 0; i < o->files; i++)
      pread(demo_fd[i], demo_buf[i], demo_bytes, 0);
  } else {
    for (int i = 0; i < o->files; i++)
      memset(demo_buf[i], 0, demo_bytes);
    demo_cold();
  }
  /* The snapshot also lets the runtime observe the previous run's
   * completions, so every run starts from the same state. */
  struct pragma_snap start;
  pragma_snap(&start);
  demo_start();
  struct overlap_run r;
  r.hash = fn(o->files);
  r.ms = demo_elapsed();
  r.waits = pragma_since(&start).waits;
  return r;
}

static inline void time_run(struct overlap* o, int pass, int which,
                            overlap_fn fn) {
  o->run[which][pass] = overlap_time(o, which, fn);
}

/* Sizes hash_rounds so hashing costs about as much as the blocking reads,
 * then records the reference hash. Two points, because each word also costs
 * something besides its rounds. */
static inline void overlap_calibrate(struct overlap* o, overlap_fn reads_only,
                                     overlap_fn hash_only) {
  if (o->fixed_rounds) {
    hash_rounds = o->fixed_rounds;
  } else {
    double reads[3];
    for (int i = 0; i < 3; i++)
      reads[i] = overlap_time(o, READS_ONLY, reads_only).ms;
    double reads_ms = pragma_median(reads, 3);
    hash_rounds = 16;
    double at16 = overlap_time(o, HASH_ONLY, hash_only).ms;
    hash_rounds = 64;
    double at64 = overlap_time(o, HASH_ONLY, hash_only).ms;
    double per_round = (at64 - at16) / 48;
    double base = at16 - 16 * per_round;
    hash_rounds = (int)((reads_ms - base) / (per_round > 0 ? per_round : 1e-9) + 0.5);
    if (hash_rounds < 1)
      hash_rounds = 1;
  }
  o->expect = overlap_time(o, HASH_ONLY, hash_only).hash;
}

static inline int overlap_report(struct overlap* o) {
  double med[N_OVERLAP_RUNS];
  printf("  %-32s %8s\n", "run", "ms");
  for (int w = 0; w < N_OVERLAP_RUNS; w++) {
    double v[PRAGMA_MAX_PASSES];
    for (int i = 0; i < o->passes; i++)
      v[i] = o->run[w][i].ms;
    med[w] = pragma_median(v, o->passes);
    printf("  %-32s %8.2f\n", overlap_name[w], med[w]);
  }

  /* B's sleeps waiting for a read: the most over any pass. */
  unsigned long b_sleeps = 0;
  int hashes_ok = 1;
  for (int i = 0; i < o->passes; i++) {
    if (o->run[ASYNC_THEN_HASH][i].waits > b_sleeps)
      b_sleeps = o->run[ASYNC_THEN_HASH][i].waits;
    hashes_ok &= o->run[READ_THEN_HASH][i].hash == o->expect &&
                 o->run[ASYNC_THEN_HASH][i].hash == o->expect;
  }

  double a = med[READ_THEN_HASH];
  double b = med[ASYNC_THEN_HASH];
  printf("\n  => %.2fx faster than blocking; the hash loop waited on the "
         "device %lu times\n",
         ratio(a, b), b_sleeps);

  pragma_check("A and B hashed the same bytes as the reference", hashes_ok);
  pragma_check_bodies();
  if (o->regime_ok) {
    pragma_check("B beat A on an uncached device", b < a);
    pragma_check("B's hash loop never slept waiting for a read", b_sleeps == 0);
  } else {
    printf("  (timings not checked: dropping the cache added no device latency)\n");
  }

  demo_finish();
  return pragma_finish();
}

/* ---- demo_pragma_wait-all ---- */

#define WAITALL_READS 30

static const char waitall_file[] = "abcdefghijklmnopqrstuvwxyz1234";

struct waitall {
  char path[PRAGMA_PATH_MAX];
  int fd;
  char* byte[WAITALL_READS];    /* one object per read, so one mark each */
  prov_tag tag[WAITALL_READS];  /* the same objects, as wait_all takes them */
  struct pragma_snap start;
};

/* Writes the file the reads will fetch from and allocates a buffer for each. */
static inline struct waitall waitall_setup(int argc, char** argv) {
  struct waitall w = {0};
  pragma_title("wait-all: one write behind thirty reads",
               "  wait_all() joins the reads' buffers; the write waits for the group.");

  snprintf(w.path, sizeof(w.path), "%s/demo_pragma_wait_all.dat",
           pragma_dir(argc, argv));
  w.fd = open(w.path, O_CREAT | O_TRUNC | O_RDWR, 0644);
  if (w.fd < 0 || pwrite(w.fd, waitall_file, WAITALL_READS, 0) != WAITALL_READS ||
      fsync(w.fd) != 0) {
    perror(w.path);
    exit(1);
  }
  for (int i = 0; i < WAITALL_READS; i++)
    w.tag[i] = w.byte[i] = (char*)prov_alloc();

  /* The first request sets up the io_uring ring. Pay for that untimed. */
  char* warm = (char*)prov_alloc();
  pragma_wait(async_pread(w.fd, warm, 1, 0));
  pragma_snap(&w.start);
  return w;
}

/* Waits for everything the demo issued, then prints and checks the outcome. */
static inline int waitall_report(struct waitall* w, void** reads, void* group,
                                 void* writer) {
  /* Before any wait: nothing has run, so the group is still pending. */
  int pending = filc_async_is_pending(group);
  struct pragma_snap issued = pragma_since(&w->start);

  int reads_ok = 1;
  for (int i = 0; i < WAITALL_READS; i++)
    reads_ok &= pragma_wait(reads[i]) == 1;
  long wrote = pragma_wait(writer);

  /* Compiled code, so the compiler's access check resolves each buffer. */
  char seen[WAITALL_READS + 1] = {0};
  int old_bytes = 1;
  for (int i = 0; i < WAITALL_READS; i++) {
    seen[i] = *w->byte[i];
    old_bytes &= seen[i] == waitall_file[i];
  }
  char first = 0;
  pread(w->fd, &first, 1, 0);

  printf("  issued: %lu calls (30 reads, 1 write); the reads reached the kernel "
         "in %lu submit\n",
         issued.calls, issued.submits);
  printf("  group pending when the write was issued: %s\n", pending ? "yes" : "no");
  printf("\n  the reads saw   %s\n", seen);
  printf("  the file holds  %c%s\n", first, waitall_file + 1);
  printf("\n  => the write landed after all 30 reads: none of them saw '!'\n");

  pragma_check_bodies();
  pragma_check("the group was pending until the reads were waited for", pending);
  pragma_check("issuing the write sent all 30 reads to the kernel in one submit",
               issued.submits == 1 && issued.waits == 0);
  pragma_check("all 30 reads and the write succeeded", reads_ok && wrote == 1);
  pragma_check("no read saw the byte the write stored", old_bytes);
  pragma_check("the write reached the file", first == '!');
  pragma_check("the group is done once the write is", !filc_async_is_pending(group));

  close(w->fd);
  unlink(w->path);
  return pragma_finish();
}

/* ---- demo_pragma_slowdown ---- */

#define CHAIN_RECORD 4096
#define CHAIN_MAX_RECORDS 1024

/* One record of the chain file: the offset of the next record, then data. */
struct record {
  long next;
  char data[CHAIN_RECORD - sizeof(long)];
};
_Static_assert(sizeof(struct record) == CHAIN_RECORD, "one record per block");

enum { CHAIN_BLOCKING, CHAIN_ANNOTATED, KNOWN_ANNOTATED, N_CHAIN_WAYS };

static const char* const chain_name[N_CHAIN_WAYS] = {
    "A chain, blocking pread",
    "B chain, async_pread",
    "C known offsets, async_pread",
};

typedef long (*chain_fn)(int n);

static char chain_path[PRAGMA_PATH_MAX];
static int chain_fd;
static long first_record;                    /* where the chain starts */
static long chain_offset[CHAIN_MAX_RECORDS]; /* the chain, in order */
static struct record* records[CHAIN_MAX_RECORDS];     /* one per record read */

struct chain_run {
  double ms;
  long sum;
  unsigned long entries; /* io_uring_enter calls: submits + waits */
};

struct chain {
  int records;
  int passes;
  long expect; /* the sum of every next offset along the chain */
  struct chain_run run[N_CHAIN_WAYS][2][PRAGMA_MAX_PASSES]; /* [way][cold] */
};

/* Writes `records` records linked in a shuffled order, so the chain jumps
 * around the file. Readahead is turned off for the file: the chain's next
 * read is never the next block, and C must not get blocks A had to wait for. */
static inline struct chain chain_setup(int argc, char** argv) {
  struct chain c = {0};
  c.records = argc > 2 ? atoi(argv[2]) : 512;
  c.passes = argc > 3 ? atoi(argv[3]) : 5;
  if (c.records < 2 || c.records > CHAIN_MAX_RECORDS || c.passes < 1 ||
      c.passes > PRAGMA_MAX_PASSES) {
    fprintf(stderr, "usage: %s [dir] [records 2..%d] [passes 1..%d]\n",
            argv[0], CHAIN_MAX_RECORDS, PRAGMA_MAX_PASSES);
    exit(2);
  }
  pragma_title("slowdown: annotating code that cannot overlap",
               "  B's next offset is in the record it just read, so its reads "
               "run one at a time.\n  An annotation pays only when there is "
               "independent work and device latency.");
  printf("  Read %d records of %d bytes, median of %d passes, from a warm and "
         "from a\n  dropped page cache. C makes B's calls with the offsets "
         "known up front.\n\n",
         c.records, CHAIN_RECORD, c.passes);

  int order[CHAIN_MAX_RECORDS];
  unsigned int state = 11;
  for (int i = 0; i < c.records; i++)
    order[i] = i;
  for (int i = c.records - 1; i > 0; i--) {
    int j = (int)(demo_rand(&state) % (unsigned)(i + 1));
    int t = order[i];
    order[i] = order[j];
    order[j] = t;
  }
  for (int i = 0; i < c.records; i++)
    chain_offset[i] = (long)order[i] * CHAIN_RECORD;
  first_record = chain_offset[0];

  snprintf(chain_path, sizeof(chain_path), "%s/demo_pragma_slowdown.dat",
           pragma_dir(argc, argv));
  chain_fd = open(chain_path, O_CREAT | O_TRUNC | O_RDWR, 0644);
  struct record* record = (struct record*)calloc(1, sizeof(*record));
  if (chain_fd < 0 || !record) {
    perror(chain_path);
    exit(1);
  }
  for (int i = 0; i < c.records; i++) {
    /* the last record points back at the first */
    record->next = chain_offset[(i + 1) % c.records];
    memset(record->data, 'a' + i % 26, sizeof(record->data));
    c.expect += record->next;
    if (pwrite(chain_fd, record, sizeof(*record), (off_t)chain_offset[i]) !=
        CHAIN_RECORD) {
      perror(chain_path);
      exit(1);
    }
  }
  free(record);
  if (fsync(chain_fd) != 0) {
    perror(chain_path);
    exit(1);
  }
  posix_fadvise(chain_fd, 0, 0, POSIX_FADV_RANDOM);
  for (int i = 0; i < c.records; i++)
    records[i] = (struct record*)calloc(1, sizeof(struct record));

  /* The first request sets up the io_uring ring; keep that out of the passes. */
  pragma_wait(async_pread(chain_fd, records[0], CHAIN_RECORD, first_record));
  return c;
}

/* Times one pass of `fn` from empty buffers, with the page cache warm or
 * dropped. */
static inline void chain_time(struct chain* c, int pass, int cold, int way,
                              chain_fn fn) {
  if (cold)
    posix_fadvise(chain_fd, 0, 0, POSIX_FADV_DONTNEED);
  else
    for (int i = 0; i < c->records; i++) /* bring every record into the cache */
      if (pread(chain_fd, records[i], CHAIN_RECORD, chain_offset[i]) !=
          CHAIN_RECORD)
        exit(1);
  for (int i = 0; i < c->records; i++)
    memset(records[i], 0, CHAIN_RECORD);

  struct chain_run* r = &c->run[way][cold][pass];
  struct pragma_snap start;
  pragma_snap(&start);
  demo_start();
  r->sum = fn(c->records);
  r->ms = demo_elapsed();
  struct pragma_snap used = pragma_since(&start);
  r->entries = way == CHAIN_BLOCKING ? (unsigned long)c->records
                                     : used.submits + used.waits;
}

static inline double chain_median(struct chain* c, int way, int cold,
                                  int entries) {
  double v[PRAGMA_MAX_PASSES];
  for (int i = 0; i < c->passes; i++)
    v[i] = entries ? (double)c->run[way][cold][i].entries
                   : c->run[way][cold][i].ms;
  return pragma_median(v, c->passes);
}

static inline int chain_report(struct chain* c) {
  double ms[N_CHAIN_WAYS][2];
  for (int w = 0; w < N_CHAIN_WAYS; w++)
    for (int cold = 0; cold <= 1; cold++)
      ms[w][cold] = chain_median(c, w, cold, 0);

  /* vs A: above 1x is faster than A, below 1x slower. kernel entries: one
   * per pread for A; io_uring_enter submits + waits for B and C. */
  printf("  %-30s %9s %7s %9s %7s %15s\n", "way", "warm ms", "vs A", "cold ms",
         "vs A", "kernel entries");
  for (int w = 0; w < N_CHAIN_WAYS; w++)
    printf("  %-30s %9.2f %6.2fx %9.2f %6.2fx %15.0f\n", chain_name[w],
           ms[w][0], ratio(ms[CHAIN_BLOCKING][0], ms[w][0]), ms[w][1],
           ratio(ms[CHAIN_BLOCKING][1], ms[w][1]),
           chain_median(c, w, 1, 1));

  printf("\n  => cold: annotating the chain gained nothing (%.2fx); the same "
         "calls with\n     the offsets known up front ran %.2fx faster than "
         "blocking\n",
         ratio(ms[CHAIN_BLOCKING][1], ms[CHAIN_ANNOTATED][1]),
         ratio(ms[CHAIN_BLOCKING][1], ms[KNOWN_ANNOTATED][1]));
  printf("  => warm: with no device latency to hide, annotating only adds cost: "
         "B ran\n     %.2fx slower than blocking, C %.2fx slower\n",
         ratio(ms[CHAIN_ANNOTATED][0], ms[CHAIN_BLOCKING][0]),
         ratio(ms[KNOWN_ANNOTATED][0], ms[CHAIN_BLOCKING][0]));

  int sums_ok = 1;
  for (int w = 0; w < N_CHAIN_WAYS; w++)
    for (int cold = 0; cold <= 1; cold++)
      for (int i = 0; i < c->passes; i++)
        sums_ok &= c->run[w][cold][i].sum == c->expect;
  pragma_check("every pass of every way read every record", sums_ok);
  pragma_check_bodies();
  pragma_check("B entered the kernel at least once per record: no batching",
               chain_median(c, CHAIN_ANNOTATED, 1, 1) >= c->records);
  pragma_check("C entered the kernel far less than once per record",
               chain_median(c, KNOWN_ANNOTATED, 1, 1) < c->records / 4.0 + 1);
  pragma_check("B was slower than A on a warm cache",
               ms[CHAIN_ANNOTATED][0] > ms[CHAIN_BLOCKING][0]);
  /* Dropping the cache must add device latency, or there is nothing for C
   * to overlap. */
  if (ms[CHAIN_BLOCKING][1] > 1.5 * ms[CHAIN_BLOCKING][0]) {
    pragma_check("B gained nothing on an uncached device",
                 ms[CHAIN_ANNOTATED][1] > 0.9 * ms[CHAIN_BLOCKING][1]);
    pragma_check("C beat A on an uncached device",
                 ms[KNOWN_ANNOTATED][1] < ms[CHAIN_BLOCKING][1]);
  } else
    printf("  (C vs A not checked: dropping the cache added no device latency)\n");

  for (int i = 0; i < c->records; i++)
    free(records[i]);
  close(chain_fd);
  unlink(chain_path);
  return pragma_finish();
}
