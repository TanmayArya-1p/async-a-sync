#pragma once

// Shared by both versions of every comparison: the files, the cold cache,
// the timed passes and the result line. Plain C, built by filcc for our
// version and by the host cc for the baseline, so both run the same
// workload the same way.

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

#define MAX_FILES 1024 // our runtime's in-flight table; neither side queues deeper
#define MAX_PASSES 15

// The files a task reads: each open for reading, each with its own buffer.
struct files {
  int n;
  size_t bytes;
  int fd[MAX_FILES];
  unsigned char* buf[MAX_FILES]; // one object each
  char path[MAX_FILES][160];
  uint64_t expect; // sum of every file's checksum
};

typedef uint64_t (*task_fn)(struct files* f);

struct bench {
  const char* task;
  const char* version;
  struct files f;
  int passes;
  double ms[MAX_PASSES];       // each cold pass
  double checksum_ms[MAX_PASSES]; // the checksum alone, data in memory
  int ok;                      // every pass returned the expected sum
};

// FNV-1a over the bytes. Compiled code: under our compiler, reading a
// buffer still being filled waits for it.
static inline uint64_t fnv1a(const unsigned char* p, size_t len) {
  uint64_t hash = 14695981039346656037ull;
  for (size_t i = 0; i < len; i++)
    hash = (hash ^ p[i]) * 1099511628211ull;
  return hash;
}

static inline double now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

static inline double median(const double* v, int n) {
  double s[MAX_PASSES];
  memcpy(s, v, sizeof(double) * (size_t)n);
  for (int i = 1; i < n; i++)
    for (int j = i; j > 0 && s[j - 1] > s[j]; j--) {
      double t = s[j];
      s[j] = s[j - 1];
      s[j - 1] = t;
    }
  return n % 2 ? s[n / 2] : (s[n / 2 - 1] + s[n / 2]) / 2.0;
}

static inline int clamp(long v, long lo, long hi) {
  return (int)(v < lo ? lo : v > hi ? hi : v);
}

// Keeping every file open can need more than the usual 1024 fds.
static inline void raise_fd_limit(void) {
  struct rlimit rl;
  if (getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur < rl.rlim_max) {
    rl.rlim_cur = rl.rlim_max;
    setrlimit(RLIMIT_NOFILE, &rl);
  }
}

// Writes the files: the same bytes for every version. Exits on failure.
static inline void make_files(struct files* f, const char* dir, const char* task) {
  raise_fd_limit();
  unsigned char* text = (unsigned char*)malloc(f->bytes);
  f->expect = 0;
  for (int i = 0; i < f->n; i++) {
    for (size_t j = 0; j < f->bytes; j++)
      text[j] = (unsigned char)(i * 131 + j * 7 + (j >> 8));
    f->expect += fnv1a(text, f->bytes);
    snprintf(f->path[i], sizeof(f->path[i]), "%s/compare_%s_%04d.dat", dir, task, i);
    int w = open(f->path[i], O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (w < 0 || write(w, text, f->bytes) != (ssize_t)f->bytes || fsync(w) != 0) {
      perror(f->path[i]);
      exit(1);
    }
    close(w);
    f->fd[i] = open(f->path[i], O_RDONLY);
    f->buf[i] = (unsigned char*)calloc(1, f->bytes);
    if (f->fd[i] < 0 || !f->buf[i]) {
      perror(f->path[i]);
      exit(1);
    }
  }
  free(text);
}

// Usage: <program> [dir] [files] [bytes] [passes]
static inline struct bench setup(int argc, char** argv, const char* task,
                                 const char* version) {
  struct bench b = {0};
  b.task = task;
  b.version = version;
  b.f.n = clamp(argc > 2 ? atol(argv[2]) : 512, 1, MAX_FILES);
  b.f.bytes = (size_t)clamp(argc > 3 ? atol(argv[3]) : 4096, 1, 1 << 20);
  b.passes = clamp(argc > 4 ? atol(argv[4]) : 5, 1, MAX_PASSES);
  printf("\n%s, %s: %d cold files of %zu bytes, median of %d passes\n", task,
         version, b.f.n, b.f.bytes, b.passes);
  make_files(&b.f, argc > 1 ? argv[1] : "/tmp", task);
  return b;
}

// Empties the buffers and drops the files from the page cache, so the next
// pass reads every byte from the device.
static inline void go_cold(struct files* f) {
  for (int i = 0; i < f->n; i++) {
    memset(f->buf[i], 0, f->bytes);
    posix_fadvise(f->fd[i], 0, (off_t)f->bytes, POSIX_FADV_DONTNEED);
  }
}

// One untimed warm-up pass, then the timed cold passes. After each pass,
// times the checksum alone over the bytes now in memory.
static inline void run_passes(struct bench* b, task_fn task) {
  b->ok = task(&b->f) == b->f.expect;
  for (int p = 0; p < b->passes; p++) {
    go_cold(&b->f);
    double t0 = now_ms();
    uint64_t sum = task(&b->f);
    b->ms[p] = now_ms() - t0;
    b->ok &= sum == b->f.expect;

    t0 = now_ms();
    sum = 0;
    for (int i = 0; i < b->f.n; i++)
      sum += fnv1a(b->f.buf[i], b->f.bytes);
    b->checksum_ms[p] = now_ms() - t0;
    b->ok &= sum == b->f.expect;
  }
}

// Prints the passes and the line run_comparison.sh reads; removes the files.
static inline int report(struct bench* b) {
  printf("  passes:");
  for (int p = 0; p < b->passes; p++)
    printf(" %.2f", b->ms[p]);
  printf(" ms\n  median %.2f ms, of which the checksum alone takes %.2f ms\n",
         median(b->ms, b->passes), median(b->checksum_ms, b->passes));
  printf("  every pass read every file's bytes: %s\n", b->ok ? "yes" : "NO");
  printf("RESULT %s %d %zu %.3f %.3f %s\n", b->task, b->f.n, b->f.bytes,
         median(b->ms, b->passes), median(b->checksum_ms, b->passes),
         b->ok ? "ok" : "FAILED");

  for (int i = 0; i < b->f.n; i++) {
    close(b->f.fd[i]);
    unlink(b->f.path[i]);
    free(b->f.buf[i]);
  }
  return b->ok ? 0 : 1;
}
