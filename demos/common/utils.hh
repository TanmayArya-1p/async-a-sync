#pragma once

// Shared by every demo: test files, the clock, word counting, and the
// title, checks and verdict each demo prints.

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

#ifdef FASYNC_IMPLICIT
#include "fasync.h"
#ifndef FASYNC_COMPILER_INSERTS_CHECKS
#error "the implicit backend needs the patched compiler: \
build with -DFASYNC_COMPILER_INSERTS_CHECKS"
#endif
#endif

// ---- test files ----

#define MAX_FILES 2048

// Files of random words, each open for reading, each with its own buffer.
static struct {
  int n;
  size_t bytes;
  int fd[MAX_FILES];
  unsigned char* buf[MAX_FILES];
  size_t words[MAX_FILES]; // words written to each file
  char path[MAX_FILES][128];
} files __attribute__((unused));

static inline unsigned next_random(unsigned* state) {
  *state = *state * 1103515245u + 12345u;
  return *state >> 16;
}

// Fills p with random words; returns how many.
static inline size_t fill_words(unsigned char* p, size_t cap, unsigned* state) {
  static const char* const words[] = {"alpha", "beta", "gamma", "delta",
                                      "epsilon"};
  size_t n = 0, count = 0;
  while (n < cap) {
    const char* w = words[next_random(state) % 5];
    size_t len = strlen(w);
    if (n + len + (count ? 1 : 0) > cap)
      break;
    if (count)
      p[n++] = (next_random(state) & 8) ? '\n' : ' ';
    memcpy(p + n, w, len);
    n += len;
    count++;
  }
  memset(p + n, ' ', cap - n);
  return count;
}

// Keeping every file open needs more than the usual 1024 fds.
static inline void raise_fd_limit(void) {
  struct rlimit rl;
  if (getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur < rl.rlim_max) {
    rl.rlim_cur = rl.rlim_max;
    setrlimit(RLIMIT_NOFILE, &rl);
  }
}

// Writes n files of `bytes` random words into dir. Exits on failure.
static inline void make_files(const char* dir, int n, size_t bytes) {
  if (n < 1 || n > MAX_FILES) {
    fprintf(stderr, "make_files: %d files, at most %d\n", n, MAX_FILES);
    exit(2);
  }
  raise_fd_limit();
  files.n = n;
  files.bytes = bytes;
  unsigned state = 7;
  unsigned char* text = (unsigned char*)malloc(bytes);
  for (int i = 0; i < n; i++) {
    files.words[i] = fill_words(text, bytes, &state);
    snprintf(files.path[i], sizeof(files.path[i]), "%s/demo_%04d.txt", dir, i);
    int w = open(files.path[i], O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (w < 0 || pwrite(w, text, bytes, 0) != (ssize_t)bytes || fsync(w) != 0) {
      perror(files.path[i]);
      exit(1);
    }
    close(w);
    files.buf[i] = (unsigned char*)calloc(1, bytes);
    files.fd[i] = open(files.path[i], O_RDONLY);
    if (!files.buf[i] || files.fd[i] < 0) {
      perror(files.path[i]);
      exit(1);
    }
  }
  free(text);
}

// Drops the files from the page cache, so each read goes to the device.
static inline void drop_cache(void) {
  for (int i = 0; i < files.n; i++)
    posix_fadvise(files.fd[i], 0, files.bytes, POSIX_FADV_DONTNEED);
}

// Words written to the first n files.
static inline size_t words_in_files(int n) {
  size_t words = 0;
  for (int i = 0; i < n; i++)
    words += files.words[i];
  return words;
}

static inline void remove_files(void) {
  for (int i = 0; i < files.n; i++) {
    close(files.fd[i]);
    unlink(files.path[i]);
    free(files.buf[i]);
  }
}

// ---- clock ----

static inline double now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

static inline int compare_doubles(const void* a, const void* b) {
  double x = *(const double*)a, y = *(const double*)b;
  return (x > y) - (x < y);
}

static inline double median(double* v, int n) {
  qsort(v, (size_t)n, sizeof(double), compare_doubles);
  return n % 2 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2.0;
}

// ---- reading the buffers ----
// Compiled code, so with the patched compiler each read of a pending
// buffer waits for it. libc's memcmp or strlen would not.

// Words in one file's buffer. noinline: inspect_disasm.sh disassembles it.
__attribute__((noinline, unused))
static size_t count_words(const unsigned char* p) {
  size_t words = 0;
  int in_word = 0;
  for (size_t i = 0; i < files.bytes; i++) {
    int separator = (p[i] == ' ' || p[i] == '\n');
    if (!separator && !in_word)
      words++;
    in_word = !separator;
  }
  return words;
}

static inline int equal(const void* a, const void* b, size_t n) {
  const unsigned char* x = (const unsigned char*)a;
  const unsigned char* y = (const unsigned char*)b;
  for (size_t i = 0; i < n; i++)
    if (x[i] != y[i])
      return 0;
  return 1;
}

// ---- title, checks, verdict ----
// Checks are recorded as a demo runs. finish() prints one line for all of
// them and names only the ones that failed.

#define MAX_CHECKS 32
static struct {
  int n, failed;
  const char* what[MAX_CHECKS];
  int ok[MAX_CHECKS];
} checks __attribute__((unused));

static inline void check(const char* what, int ok) {
  if (checks.n < MAX_CHECKS) {
    checks.what[checks.n] = what;
    checks.ok[checks.n] = ok;
    checks.n++;
  }
  if (!ok)
    checks.failed++;
}

static inline void title(const char* name, const char* subtitle) {
  printf("\n%s\n", name);
  for (size_t i = 0; i < strlen(name); i++)
    putchar('-');
  printf("\n");
  if (subtitle)
    printf("%s\n\n", subtitle);
}

static inline int finish(void) {
  printf("  checks: %d/%d passed\n", checks.n - checks.failed, checks.n);
  for (int i = 0; i < checks.n; i++)
    if (!checks.ok[i])
      printf("    FAILED: %s\n", checks.what[i]);
  printf("\n%s\n", checks.failed ? "DEMO FAILED" : "DEMO OK");
  return checks.failed ? 1 : 0;
}
