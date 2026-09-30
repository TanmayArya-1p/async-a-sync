#pragma once

// Blocks, steps, setup and report for demo_async_io.

#include "explicit_utils.hh"

#define BLOCKS 64
#define BLOCK_SIZE (256 * 1024)

struct demo {
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
static inline void next_step(struct demo* d) {
  for (int i = 0; i < BLOCKS; i++)
    memset(d->block[i], 0, BLOCK_SIZE);
  posix_fadvise(d->fd, 0, 0, POSIX_FADV_DONTNEED);
  steps.start = io_stats();
  steps.t0 = now_ms();
}

// Writes one file of BLOCKS blocks and opens it.
static inline struct demo setup(int argc, char** argv) {
  struct demo d = {0};
  title("async_io: the explicit fasync API on one file",
        "  64 blocks of 256 KiB, read three ways.");
  snprintf(d.path, sizeof(d.path), "%s/demo_async_io.bin", dir_arg(argc, argv));
  int w = open(d.path, O_CREAT | O_TRUNC | O_WRONLY, 0644);
  for (int i = 0; i < BLOCKS; i++) {
    d.block[i] = (unsigned char*)malloc(BLOCK_SIZE);
    memset(d.block[i], i + 1, BLOCK_SIZE);
    if (w < 0 || pwrite(w, d.block[i], BLOCK_SIZE, (off_t)i * BLOCK_SIZE) != BLOCK_SIZE) {
      perror(d.path);
      exit(1);
    }
  }
  fsync(w);
  close(w);
  d.fd = open(d.path, O_RDONLY);
  if (d.fd < 0) {
    perror(d.path);
    exit(1);
  }
  printf("  %-30s %8s %6s %15s   %s\n", "step", "ms", "reads",
         "kernel entries", "outcome");
  next_step(&d);
  return d;
}

// Prints the step main just ran.
static inline void step(struct demo* d, const char* what, int ok) {
  double ms = now_ms() - steps.t0;
  struct fasync_stats s = io_stats();
  unsigned long reads = s.sqes_queued - steps.start.sqes_queued;
  unsigned long entries = s.kernel_submit_entries + s.kernel_wait_entries -
                          steps.start.kernel_submit_entries -
                          steps.start.kernel_wait_entries;
  printf("  %-30s %8.2f %6lu %15lu   %s\n", what, ms, reads, entries,
         ok ? "ok" : "FAILED");
  check(what, ok);
  next_step(d);
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

static inline int report(struct demo* d) {
  printf("\n");
  close(d->fd);
  unlink(d->path);
  for (int i = 0; i < BLOCKS; i++)
    free(d->block[i]);
  return finish();
}
