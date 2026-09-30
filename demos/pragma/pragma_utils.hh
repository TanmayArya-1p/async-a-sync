#pragma once

// Logging, setup and reports for the pragma demos, so each demo file holds
// only its annotated functions and their calls.

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

// ---- demo_pragma_hello ----

static const char hello_text[] =
    "Hello from io_uring: these bytes were read by an annotated call.";

struct hello {
  char path[256];
  int fd;
  char* buf;
  size_t len;
  double t0;
  struct counters start;
};

static inline struct hello hello_setup(int argc, char** argv) {
  struct hello h = {0};
  title("hello: one annotated call",
        "  read_block() is plain C; its pragma says op=pread.");
  h.fd = make_file(h.path, sizeof(h.path), dir_arg(argc, argv),
                   "demo_pragma_hello.txt", hello_text);
  h.len = strlen(hello_text);
  h.buf = (char*)calloc(1, 128);
  warm_up(h.fd);
  posix_fadvise(h.fd, 0, h.len, POSIX_FADV_DONTNEED);
  h.start = counters_now();
  h.t0 = now_ms();
  return h;
}

// Called right after main read buf[0].
static inline int hello_report(struct hello* h, char first) {
  double read_ms = now_ms() - h->t0;
  struct counters used = counters_since(h->start);
  unsigned long entries_at_call =
      calls.n ? calls.call[0].entries - (h->start.submits + h->start.waits) : 0;

  printf("  %-22s %9s %15s\n", "step", "time", "kernel entries");
  printf("  %-22s %6.3f ms %15lu\n", "read_block() called",
         calls.n ? calls.call[0].ms - h->t0 : 0, entries_at_call);
  printf("  %-22s %6.3f ms %15lu\n", "buf[0] read", read_ms,
         used.submits + used.waits);
  printf("\n  buf[0] is '%c'; buf holds:\n  \"%s\"\n", first, h->buf);
  printf("\n  => one call, one request; reading buf sent it and waited for it\n");

  check_bodies();
  check("reading buf found it pending and waited", used.hook_waits == 1);
  check("the first read of buf sent the request", used.submits == 1);
  check("buf holds the file's bytes",
        first == hello_text[0] && equal(h->buf, hello_text, h->len));

  free(h->buf);
  close(h->fd);
  unlink(h->path);
  return finish();
}

// ---- demo_pragma_lifecycle ----

struct lifecycle {
  char path[256];
  const char* text;
  size_t len;
  double t0;
  struct counters start;
};

static inline struct lifecycle lifecycle_setup(int argc, char** argv,
                                               const char* text) {
  struct lifecycle l = {0};
  title("lifecycle: open, write, fsync, read, close",
        "  Five annotated calls, issued with no wait between them.");
  snprintf(l.path, sizeof(l.path), "%s/demo_pragma_lifecycle.txt",
           dir_arg(argc, argv));
  unlink(l.path);
  l.text = text;
  l.len = strlen(text);
  l.start = counters_now();
  l.t0 = now_ms();
  return l;
}

// Waits for the four calls after the open, then checks the file.
static inline int lifecycle_report(struct lifecycle* l, int fd, const char* back,
                                   void* wrote, void* synced, void* read_back,
                                   void* closed) {
  long wrote_n = wait_for(wrote);
  long synced_n = wait_for(synced);
  long read_n = wait_for(read_back);
  long closed_n = wait_for(closed);
  double ms = now_ms() - l->t0;
  struct counters used = counters_since(l->start);

  // The log holds the five calls in the order they ran.
  char result[5][96];
  snprintf(result[0], sizeof(result[0]), "fd %d", fd);
  snprintf(result[1], sizeof(result[1]), "%ld bytes written", wrote_n);
  snprintf(result[2], sizeof(result[2]), "%ld", synced_n);
  snprintf(result[3], sizeof(result[3]), "%ld bytes: \"%s\"", read_n, back);
  snprintf(result[4], sizeof(result[4]), "%ld", closed_n);
  printf("  %-14s %8s   %s\n", "call", "ran at", "result");
  for (int i = 0; i < calls.n && i < 5; i++)
    printf("  %-14s %5.2f ms   %s\n", calls.call[i].name,
           calls.call[i].ms - l->t0, result[i]);
  printf("\n  %lu calls, %lu io_uring requests, %lu kernel entries, %.2f ms\n",
         used.calls, used.sqes, used.submits + used.waits, ms);
  printf("\n  => 5 annotated syscalls ran in program order, no wait between "
         "them\n");

  // Check the file with ordinary blocking calls.
  char disk[128] = {0};
  int check_fd = open(l->path, O_RDONLY);
  ssize_t disk_n = check_fd >= 0 ? read(check_fd, disk, sizeof(disk)) : -1;
  if (check_fd >= 0)
    close(check_fd);
  errno = 0;
  int fd_closed = fcntl(fd, F_GETFD) == -1 && errno == EBADF;

  check("every call succeeded",
        fd >= 0 && wrote_n == (long)l->len && synced_n == 0 &&
            read_n == (long)l->len && closed_n == 0);
  check("the read saw the write issued before it", equal(back, l->text, l->len));
  check("the file holds the text (checked with plain read)",
        disk_n == (ssize_t)l->len && equal(disk, l->text, l->len));
  check("the fd was closed by the last call", fd_closed);
  check("five calls, five requests, five bodies run",
        used.calls == 5 && used.sqes == 5 && calls.n == 5);

  unlink(l->path);
  return finish();
}

// ---- demo_pragma_ordering ----

enum { FILES = 8, RECORD = 16 };

// What a scenario's calls should do while they are issued.
enum { BATCHES, WAITS };

struct ordering {
  int fd[FILES];         // the files the writes go to
  int records;           // a file of FILES numbered records
  char* record[FILES];   // one buffer per read
  char path[FILES + 1][256];
};

struct scenario {
  const char* what;
  int calls;
  int waited;           // a call waited before its body ran
  unsigned long submits; // io_uring_enter calls that submitted
  int ok;
};

static struct {
  int n;
  int first_call; // the first logged call of the next scenario
  struct counters start;
  struct scenario row[4];
} scenarios __attribute__((unused));

// Fills fd with numbered records for the reads scenario.
static inline void write_records(int fd) {
  char record[RECORD + 1];
  for (int i = 0; i < FILES; i++) {
    snprintf(record, sizeof(record), "record number %02d", i);
    if (pwrite(fd, record, RECORD, (off_t)i * RECORD) != RECORD) {
      perror("write_records");
      exit(1);
    }
  }
}

static inline int is_record(const char* buf, int i) {
  char record[RECORD + 1];
  snprintf(record, sizeof(record), "record number %02d", i);
  return equal(buf, record, RECORD);
}

static inline struct ordering ordering_setup(int argc, char** argv) {
  struct ordering o = {0};
  title("ordering: dependencies order calls, the rest batch",
        "  Calls that conflict on an fd wait; independent calls batch.");
  for (int i = 0; i < FILES; i++) {
    char name[64];
    snprintf(name, sizeof(name), "demo_pragma_ordering_%d.dat", i);
    o.fd[i] = make_file(o.path[i], sizeof(o.path[i]), dir_arg(argc, argv),
                        name, "");
    o.record[i] = (char*)calloc(1, RECORD + 1);
  }
  o.records = make_file(o.path[FILES], sizeof(o.path[FILES]), dir_arg(argc, argv),
                        "demo_pragma_ordering_records.dat", "");
  write_records(o.records);
  warm_up(o.fd[0]);
  scenarios.start = counters_now();
  return o;
}

// Records a scenario from the calls logged since the last one.
static inline void scenario(const char* what, int ok, int expect) {
  struct scenario* s = &scenarios.row[scenarios.n++];
  struct counters used = counters_since(scenarios.start);
  int first = scenarios.first_call;
  int last = calls.n < LOG_MAX ? calls.n : LOG_MAX;
  s->what = what;
  s->calls = calls.n - first;
  s->waited = last > first && calls.call[last - 1].entries > calls.call[first].entries;
  s->submits = used.submits;
  s->ok = ok;

  check(what, ok);
  if (expect == WAITS) {
    check("  conflicting calls waited inside the call", s->waited);
  } else {
    check("  independent calls did not wait while issued", !s->waited);
    check("  the batch reached the kernel in one submit", s->submits == 1);
  }
  scenarios.first_call = calls.n;
  scenarios.start = counters_now();
}

static inline int ordering_report(struct ordering* o) {
  // waited: a call entered the kernel before its body ran, to wait for the
  // conflicting call before it.
  printf("  %-30s %5s %7s %8s   %s\n", "scenario", "calls", "waited",
         "submits", "outcome");
  for (int i = 0; i < scenarios.n; i++) {
    struct scenario* s = &scenarios.row[i];
    printf("  %-30s %5d %7s %8lu   %s\n", s->what, s->calls,
           s->waited ? "yes" : "no", s->submits, s->ok ? "ok" : "FAILED");
  }
  printf("\n  => conflicting calls kept their order; %d independent calls, "
         "%lu kernel submit\n",
         scenarios.row[1].calls, scenarios.row[1].submits);

  for (int i = 0; i < FILES; i++) {
    close(o->fd[i]);
    unlink(o->path[i]);
    free(o->record[i]);
  }
  close(o->records);
  unlink(o->path[FILES]);
  check_bodies();
  return finish();
}

// ---- demo_pragma_coldread and demo_pragma_scaling ----

#define READ_BYTES 4096
#define MAX_PASSES 15

typedef size_t (*read_fn)(int n);

struct pass {
  double ms;
  double issue_ms; // until the last annotated call ran
  size_t words;
  struct counters used;
};

struct timing {
  int files;
  int passes;
  int cache_matters;
  struct pass blocking[MAX_PASSES];
  struct pass annotated[MAX_PASSES];
  struct pass by_hand[MAX_PASSES];

  // scaling's running results
  int words_ok;
  int batched;
  double best;
  int best_n;
  double last_call_us;
  double last_speedup;
  int last_n;
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

// Parses [dir] [files] [passes], makes the files and warms up the ring.
static inline struct timing timing_setup(int argc, char** argv, int files_n,
                                         int passes) {
  struct timing t = {0};
  t.files = argc > 2 ? atoi(argv[2]) : files_n;
  t.passes = argc > 3 ? atoi(argv[3]) : passes;
  t.words_ok = 1;
  t.batched = 1;
  if (t.files < 1 || t.files > MAX_FILES || t.passes < 1 ||
      t.passes > MAX_PASSES) {
    fprintf(stderr, "usage: %s [dir] [files 1..%d] [passes 1..%d]\n", argv[0],
            MAX_FILES, MAX_PASSES);
    exit(2);
  }
  return t;
}

static inline void timing_files(struct timing* t, int argc, char** argv) {
  make_files(dir_arg(argc, argv), t->files, READ_BYTES);
  t->cache_matters = cache_matters();
  warm_up(files.fd[0]);
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

static inline struct timing coldread_setup(int argc, char** argv) {
  title("coldread: the same loop, blocking and annotated", 0);
  struct timing t = timing_setup(argc, argv, 512, 5);
  printf("  Word count over %d cold files of %d bytes, median of %d passes.\n"
         "  B is A's loop with pread renamed to async_pread.\n\n",
         t.files, READ_BYTES, t.passes);
  timing_files(&t, argc, argv);
  return t;
}

static inline int coldread_report(struct timing* t) {
  static const char* const name[] = {"A blocking pread",
                                     "B annotated async_pread",
                                     "C hand-written fasync_*"};
  const struct pass* way[] = {t->blocking, t->annotated, t->by_hand};
  double ms[3];
  for (int w = 0; w < 3; w++)
    ms[w] = pass_median(way[w], t->passes, MS);

  // kernel entries: one per pread for A; io_uring_enter submits and waits
  // for B and C. sleeps: times this thread blocked.
  printf("  %-26s %8s %7s %15s %7s\n", "arm", "ms", "vs A", "kernel entries",
         "sleeps");
  for (int w = 0; w < 3; w++) {
    double entries = w == 0 ? t->files : pass_median(way[w], t->passes, ENTRIES);
    printf("  %-26s %8.2f %6.2fx %15.0f %7.0f\n", name[w], ms[w],
           ratio(ms[0], ms[w]), entries, pass_median(way[w], t->passes, SLEEPS));
  }
  printf("\n  => %.2fx faster than blocking pread over %d cold files, same "
         "loop\n",
         ratio(ms[0], ms[1]), t->files);

  int words_ok = 1;
  for (int w = 0; w < 3; w++)
    words_ok &= words_counted(way[w], t->passes, t->files);
  check("every pass of every arm counted every word", words_ok);
  check_bodies();
  check("B entered the kernel far less than once per file",
        pass_median(t->annotated, t->passes, ENTRIES) < t->files / 4.0 + 1);
  if (t->cache_matters)
    check("B beat A on an uncached device", ms[1] < ms[0]);
  else
    printf("  (B vs A not checked: dropping the cache added no device latency)\n");

  remove_files();
  return finish();
}

static inline struct timing scaling_setup(int argc, char** argv) {
  title("scaling: the gain as the number of reads grows", 0);
  struct timing t = timing_setup(argc, argv, 2048, 3);
  printf("  Word count over N cold files of %d bytes, N = 1..%d, median of %d\n"
         "  passes. us/call is the time inside each annotated call.\n\n",
         READ_BYTES, t.files, t.passes);
  timing_files(&t, argc, argv);
  printf("  %6s %12s %12s %12s %8s %8s\n", "files", "A pread ms",
         "B async ms", "C by hand ms", "B vs A", "us/call");
  return t;
}

// Prints the row for n files from the passes just run.
static inline void scaling_row(struct timing* t, int n) {
  double a = pass_median(t->blocking, t->passes, MS);
  double b = pass_median(t->annotated, t->passes, MS);
  double c = pass_median(t->by_hand, t->passes, MS);
  double call_us = pass_median(t->annotated, t->passes, ISSUE_MS) * 1000.0 / n;
  double speedup = ratio(a, b);
  printf("  %6d %12.2f %12.2f %12.2f %7.2fx %8.1f\n", n, a, b, c, speedup,
         call_us);

  t->words_ok &= words_counted(t->blocking, t->passes, n) &&
                 words_counted(t->annotated, t->passes, n) &&
                 words_counted(t->by_hand, t->passes, n);
  // Below 64 files a pass is too short for the ratio to mean much.
  if (n >= 64) {
    t->batched &= pass_median(t->annotated, t->passes, ENTRIES) <= n / 4.0 + 2;
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
  // Above 1024 files the request table is full, so us/call includes waiting
  // for room in it.
  if (t->best_n)
    printf("\n  => %.2fx faster than blocking at %d files, %.2fx at %d (%.1f us "
           "per call)\n",
           t->best, t->best_n, t->last_speedup, t->last_n, t->last_call_us);

  check("every pass counted every word", t->words_ok);
  check_bodies();
  check("from 64 files up, B batched its reads into few entries", t->batched);
  if (t->cache_matters && t->best_n)
    check("B beat A at some size from 64 up on an uncached device", t->best > 1.0);
  else
    printf("  (B vs A not checked: dropping the cache added no device latency)\n");

  remove_files();
  return finish();
}

// ---- demo_pragma_overlap ----

#define OVERLAP_BYTES 4096

// How many times the hash mixes each 8-byte word. overlap_setup sizes it so
// hashing every file takes as long as reading them with blocking pread.
static int hash_rounds __attribute__((unused)) = 1;

// FNV-1a over 8-byte words, mixed hash_rounds times: costs compute, not
// memory access. noinline, so no caller sees hash_rounds as a constant:
// the rounds must cost the same while the hash is sized as after.
__attribute__((noinline)) static unsigned long hash_file(const unsigned char* p) {
  const unsigned long* w = (const unsigned long*)p;
  unsigned long h = 1469598103934665603UL;
  for (size_t i = 0; i < files.bytes / sizeof(*w); i++) {
    unsigned long x = w[i];
    for (int r = 0; r < hash_rounds; r++)
      h = (h ^ (x + (unsigned long)r)) * 1099511628211UL;
  }
  return h;
}

typedef unsigned long (*overlap_fn)(int n);

struct run {
  double ms;
  unsigned long hash;
  unsigned long waits; // io_uring_enter calls that slept
};

struct overlap {
  int passes;
  int fixed_rounds;
  int cache_matters;
  unsigned long expect;
  struct run reads_only[MAX_PASSES];
  struct run hash_only[MAX_PASSES];
  struct run blocking[MAX_PASSES]; // A: read_then_hash
  struct run async[MAX_PASSES];    // B: async_then_hash
};

// The two halves of A, timed alone.
static inline unsigned long reads_only(int n) {
  for (int i = 0; i < n; i++)
    pread(files.fd[i], files.buf[i], files.bytes, 0);
  return 0;
}

static inline unsigned long hash_only(int n) {
  unsigned long h = 0;
  for (int i = 0; i < n; i++)
    h ^= hash_file(files.buf[i]);
  return h;
}

// Times one run over every file. hash_only needs the data in memory; every
// other run starts from empty buffers and a dropped cache.
static inline void time_run(struct run* r, overlap_fn fn) {
  if (fn == hash_only) {
    reads_only(files.n);
  } else {
    for (int i = 0; i < files.n; i++)
      memset(files.buf[i], 0, files.bytes);
    drop_cache();
  }
  // The counters also let the runtime see the last run's completions.
  struct counters start = counters_now();
  double t = now_ms();
  r->hash = fn(files.n);
  r->ms = now_ms() - t;
  r->waits = counters_since(start).waits;
}

// Sizes hash_rounds so hashing costs about as much as the blocking reads.
// Two points, because each word also costs something besides its rounds.
static inline void size_hash(struct overlap* o) {
  struct run r;
  if (o->fixed_rounds) {
    hash_rounds = o->fixed_rounds;
  } else {
    double reads[3];
    for (int i = 0; i < 3; i++) {
      time_run(&r, reads_only);
      reads[i] = r.ms;
    }
    double reads_ms = median(reads, 3);
    hash_rounds = 16;
    time_run(&r, hash_only);
    double at16 = r.ms;
    hash_rounds = 64;
    time_run(&r, hash_only);
    double per_round = (r.ms - at16) / 48;
    double base = at16 - 16 * per_round;
    hash_rounds = (int)((reads_ms - base) / (per_round > 0 ? per_round : 1e-9) + 0.5);
    if (hash_rounds < 1)
      hash_rounds = 1;
  }
  time_run(&r, hash_only);
  o->expect = r.hash;
}

// Parses [dir] [files] [passes] [rounds], makes the files, sizes the hash,
// and times the reads and the hashing alone.
static inline struct overlap overlap_setup(int argc, char** argv) {
  struct overlap o = {0};
  int n = argc > 2 ? atoi(argv[2]) : 256;
  o.passes = argc > 3 ? atoi(argv[3]) : 5;
  o.fixed_rounds = argc > 4 ? atoi(argv[4]) : 0;
  if (n < 1 || n > MAX_FILES || o.passes < 1 || o.passes > MAX_PASSES ||
      o.fixed_rounds < 0) {
    fprintf(stderr, "usage: %s [dir] [files 1..%d] [passes 1..%d] [rounds]\n",
            argv[0], MAX_FILES, MAX_PASSES);
    exit(2);
  }

  title("overlap: the reads run while the program hashes", 0);
  printf("  Read %d cold files of %d bytes and hash each one, median of %d\n"
         "  passes. The hash is sized to take as long as the blocking reads.\n\n",
         n, OVERLAP_BYTES, o.passes);

  make_files(dir_arg(argc, argv), n, OVERLAP_BYTES);
  o.cache_matters = cache_matters();
  warm_up(files.fd[0]);
  size_hash(&o);
  for (int pass = 0; pass < o.passes; pass++) {
    time_run(&o.reads_only[pass], reads_only);
    time_run(&o.hash_only[pass], hash_only);
  }
  return o;
}

static inline double run_median(const struct run* runs, int n) {
  double v[MAX_PASSES];
  for (int i = 0; i < n; i++)
    v[i] = runs[i].ms;
  return median(v, n);
}

static inline int overlap_report(struct overlap* o) {
  static const char* const name[] = {
      "reads only, pread", "hashing only (data in memory)",
      "A  pread + hash, file by file", "B  async_pread all, then hash"};
  const struct run* way[] = {o->reads_only, o->hash_only, o->blocking, o->async};
  printf("  %-32s %8s\n", "run", "ms");
  for (int w = 0; w < 4; w++)
    printf("  %-32s %8.2f\n", name[w], run_median(way[w], o->passes));

  // B's sleeps waiting for a read: the most over any pass.
  unsigned long b_sleeps = 0;
  int hashes_ok = 1;
  for (int i = 0; i < o->passes; i++) {
    if (o->async[i].waits > b_sleeps)
      b_sleeps = o->async[i].waits;
    hashes_ok &= o->blocking[i].hash == o->expect && o->async[i].hash == o->expect;
  }

  double a = run_median(o->blocking, o->passes);
  double b = run_median(o->async, o->passes);
  printf("\n  => %.2fx faster than blocking; the hash loop waited on the "
         "device %lu times\n",
         ratio(a, b), b_sleeps);

  check("A and B hashed the same bytes as the reference", hashes_ok);
  check_bodies();
  if (o->cache_matters) {
    check("B beat A on an uncached device", b < a);
    check("B's hash loop never slept waiting for a read", b_sleeps == 0);
  } else {
    printf("  (timings not checked: dropping the cache added no device latency)\n");
  }

  remove_files();
  return finish();
}
