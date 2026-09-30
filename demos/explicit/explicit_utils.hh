#pragma once

// Setup, logging and reports for the explicit-API demos, so each demo file
// holds only its fasync_* calls.

#include "fasync.h"
#include "fasync_dep.h"
#include "../common/utils.hh"

static inline const char* dir_arg(int argc, char** argv) {
  return argc > 1 ? argv[1] : "/tmp";
}

// Kernel entries and requests so far.
static inline struct fasync_stats io_stats(void) {
  struct fasync_stats s;
  fasync_get_stats(&s);
  return s;
}

// ---- demo_plain_io ----

static inline void plain_io_setup(int argc, char** argv) {
  title("plain_io: ask for every file, count every file",
        "  fasync_pread queues each read. Counting a buffer waits for it.");
  make_files(dir_arg(argc, argv), 4, 512 * 1024);
  drop_cache();
}

static inline int plain_io_report(int correct) {
  struct fasync_stats s = io_stats();
  printf("  %d files of %zu KiB: %lu reads queued, %lu kernel entries\n",
         files.n, files.bytes / 1024, s.sqes_queued,
         s.kernel_submit_entries + s.kernel_wait_entries);
  printf("  %d/%d files counted right\n\n", correct, files.n);
  check("every file counted right", correct == files.n);
  remove_files();
  return finish();
}

// ---- demo_async_io ----

#define BLOCKS 64
#define BLOCK_SIZE (256 * 1024)

struct blocks {
  char path[256];
  int fd;
  unsigned char* block[BLOCKS];
};

// Block i holds the byte i + 1.
static inline int block_ok(const unsigned char* block, int i) {
  for (size_t j = 0; j < BLOCK_SIZE; j += 512)
    if (block[j] != (unsigned char)(i + 1))
      return 0;
  return 1;
}

// When the current step started.
static struct {
  double t0;
  struct fasync_stats start;
} steps __attribute__((unused));

// Readies the next step: empty buffers, a cold file, a fresh clock.
static inline void next_step(struct blocks* b) {
  for (int i = 0; i < BLOCKS; i++)
    memset(b->block[i], 0, BLOCK_SIZE);
  posix_fadvise(b->fd, 0, 0, POSIX_FADV_DONTNEED);
  steps.start = io_stats();
  steps.t0 = now_ms();
}

// Writes one file of BLOCKS blocks and opens it.
static inline struct blocks async_io_setup(int argc, char** argv) {
  struct blocks b = {0};
  title("async_io: the explicit fasync API on one file",
        "  64 blocks of 256 KiB, read three ways.");
  snprintf(b.path, sizeof(b.path), "%s/demo_async_io.bin", dir_arg(argc, argv));
  int w = open(b.path, O_CREAT | O_TRUNC | O_WRONLY, 0644);
  for (int i = 0; i < BLOCKS; i++) {
    b.block[i] = (unsigned char*)malloc(BLOCK_SIZE);
    memset(b.block[i], i + 1, BLOCK_SIZE);
    if (w < 0 || pwrite(w, b.block[i], BLOCK_SIZE, (off_t)i * BLOCK_SIZE) != BLOCK_SIZE) {
      perror(b.path);
      exit(1);
    }
  }
  fsync(w);
  close(w);
  b.fd = open(b.path, O_RDONLY);
  if (b.fd < 0) {
    perror(b.path);
    exit(1);
  }
  printf("  %-30s %8s %6s %15s   %s\n", "step", "ms", "reads",
         "kernel entries", "outcome");
  next_step(&b);
  return b;
}

// Prints the step main just ran.
static inline void step(struct blocks* b, const char* what, int ok) {
  double ms = now_ms() - steps.t0;
  struct fasync_stats s = io_stats();
  unsigned long reads = s.sqes_queued - steps.start.sqes_queued;
  unsigned long entries = s.kernel_submit_entries + s.kernel_wait_entries -
                          steps.start.kernel_submit_entries -
                          steps.start.kernel_wait_entries;
  printf("  %-30s %8.2f %6lu %15lu   %s\n", what, ms, reads, entries,
         ok ? "ok" : "FAILED");
  check(what, ok);
  next_step(b);
}

// Prints the edges fasync_build_dag found among ops.
static inline void show_dag(const struct fasync_op* op, unsigned n_ops,
                            const unsigned* edge, unsigned n,
                            const struct fasync_dag_stats* st) {
  printf("\n  %u operations, %u must wait (%u pairs proven disjoint):\n", n_ops,
         n, st->auto_disjoint_pairs);
  for (unsigned e = 0; e < n; e++)
    printf("    %s -> %s\n", op[edge[e] / n_ops].name, op[edge[e] % n_ops].name);
  check("copy(a,c) waits for write(a), read(c) waits for copy(a,c)", n == 2);
}

static inline int async_io_report(struct blocks* b) {
  printf("\n");
  close(b->fd);
  unlink(b->path);
  for (int i = 0; i < BLOCKS; i++)
    free(b->block[i]);
  return finish();
}

// ---- demo_provenance ----

// Both calls' handles.
struct pair {
  fasync_id write, read;
};

struct provenance {
  char path[256];
  int fd_write, fd_read; // two fds on one file
  size_t len;
  unsigned char* written;        // what the write puts in the file
  unsigned char* read_untracked; // the read without a tracker
  unsigned char* read_tracked;   // the read with one
};

// Puts other bytes in the file.
static inline void reset_file(struct provenance* p) {
  unsigned char* old_bytes = (unsigned char*)malloc(p->len);
  memset(old_bytes, 0xA7, p->len);
  if (pwrite(p->fd_write, old_bytes, p->len, 0) != (ssize_t)p->len) {
    perror(p->path);
    exit(1);
  }
  free(old_bytes);
}

static inline struct provenance provenance_setup(int argc, char** argv) {
  struct provenance p = {0};
  title("provenance: a write and a read through two fds",
        "  Nothing shows they touch the same bytes. A tracker orders them.");
  p.len = 128 * 1024;
  p.written = (unsigned char*)malloc(p.len);
  p.read_untracked = (unsigned char*)calloc(1, p.len);
  p.read_tracked = (unsigned char*)calloc(1, p.len);
  memset(p.written, 0x5E, p.len);
  snprintf(p.path, sizeof(p.path), "%s/demo_provenance.bin", dir_arg(argc, argv));
  p.fd_write = open(p.path, O_CREAT | O_TRUNC | O_WRONLY, 0644);
  p.fd_read = open(p.path, O_RDONLY);
  if (p.fd_write < 0 || p.fd_read < 0) {
    perror(p.path);
    exit(1);
  }
  reset_file(&p);
  return p;
}

// Without a tracker both calls are queued and neither has run.
static inline void check_untracked(struct provenance* p, struct pair c) {
  int in_flight = !fasync_ready(c.write) && !fasync_ready(c.read);
  int done = fasync_result(c.write) == (long)p->len &&
             fasync_result(c.read) == (long)p->len;
  printf("  no tracker:  both calls in flight at once: %s\n",
         in_flight ? "yes" : "no");
  check("without a tracker, nothing ordered the two calls", in_flight && done);
  reset_file(p);
}

// With a tracker the read waited for the write, and saw its bytes.
static inline void check_tracked(struct provenance* p, struct pair c) {
  int ordered = fasync_ready(c.write);
  int done = fasync_result(c.write) == (long)p->len &&
             fasync_result(c.read) == (long)p->len;
  int saw_write = equal(p->read_tracked, p->written, p->len);
  printf("  one tracker: the write finished before the read: %s; "
         "the read saw it: %s\n",
         ordered ? "yes" : "no", saw_write ? "yes" : "no");
  check("with a tracker, the read ran after the write", ordered && done);
  check("and saw the written bytes", saw_write);
}

static inline int provenance_report(struct provenance* p) {
  printf("\n");
  close(p->fd_write);
  close(p->fd_read);
  unlink(p->path);
  free(p->written);
  free(p->read_untracked);
  free(p->read_tracked);
  return finish();
}
