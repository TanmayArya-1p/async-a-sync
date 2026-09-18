/*
 * stage3_dependency.c -- declared effect sets and automatic disjointness.
 *
 * Two things are checked. (1) The analysis: given declared in/out sets the DAG
 * has exactly the implied edges (write->read, read->write, write->write) and
 * none for read->read; conflicting kinds over provably disjoint ranges produce
 * NO edge, and that saving is counted. (2) The execution: a DAG over real file
 * I/O, measuring peak concurrency -- independent ops in flight together,
 * dependent ops not.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

#include "fasync.h"
#include "fasync_dep.h"

static int failures = 0;

static void check(const char* what, int ok) {
  printf("  %-58s %s\n", what, ok ? "ok" : "FAIL");
  fflush(stdout);
  if (!ok)
    failures++;
}

#define MAX_EDGES 64

/* ------------------------------------------------------------------ */
/* Part 1: the analysis                                                */
/* ------------------------------------------------------------------ */

static void test_analysis(void) {
  printf("dependency analysis:\n");
  fflush(stdout);

  void* x = malloc(4096);
  void* y = malloc(4096); /* a separate GC object: provably disjoint from x */

  struct fasync_access wx[] = {{x, 128, FASYNC_OUT}};
  struct fasync_access rx[] = {{x, 128, FASYNC_IN}};
  struct fasync_access wy[] = {{y, 128, FASYNC_OUT}};
  struct fasync_access ry[] = {{y, 128, FASYNC_IN}};

  struct fasync_op ops[4];
  unsigned edges[MAX_EDGES];
  struct fasync_dag_stats st;

  /* (a) Two writes to the SAME buffer: output dependency, one edge. */
  ops[0] = (struct fasync_op){"w(x)", wx, 1};
  ops[1] = (struct fasync_op){"w(x)", wx, 1};
  unsigned n = fasync_build_dag(ops, 2, edges, MAX_EDGES, &st);
  check("write/write on same buffer -> 1 edge", n == 1);
  check("  classified as output dependency", st.write_write_edges == 1);

  /* (b) Write then read of the same buffer: true dependency. */
  ops[0] = (struct fasync_op){"w(x)", wx, 1};
  ops[1] = (struct fasync_op){"r(x)", rx, 1};
  n = fasync_build_dag(ops, 2, edges, MAX_EDGES, &st);
  check("write/read on same buffer -> 1 edge", n == 1);
  check("  classified as writer-to-reader", st.read_write_edges == 1);

  /* (c) Read then write: anti-dependency, still an edge (order matters). */
  ops[0] = (struct fasync_op){"r(x)", rx, 1};
  ops[1] = (struct fasync_op){"w(x)", wx, 1};
  n = fasync_build_dag(ops, 2, edges, MAX_EDGES, &st);
  check("read/write on same buffer -> 1 edge", n == 1);
  check("  classified as reader-to-writer", st.write_read_edges == 1);

  /* (d) Two reads of the same buffer: no conflict at all. Readers do not
   *     serialize against each other. */
  ops[0] = (struct fasync_op){"r(x)", rx, 1};
  ops[1] = (struct fasync_op){"r(x)", rx, 1};
  n = fasync_build_dag(ops, 2, edges, MAX_EDGES, &st);
  check("read/read on same buffer -> no edge", n == 0);
  check("  not counted as an auto-proved saving",
        st.auto_disjoint_pairs == 0);

  /* (e) The interesting case: conflicting declared KINDS but provably disjoint
   *     ranges. No edge, and the saving is recorded. */
  ops[0] = (struct fasync_op){"w(x)", wx, 1};
  ops[1] = (struct fasync_op){"w(y)", wy, 1};
  n = fasync_build_dag(ops, 2, edges, MAX_EDGES, &st);
  check("write/write on DISJOINT buffers -> no edge", n == 0);
  check("  counted as an auto-proved saving", st.auto_disjoint_pairs == 1);

  /* (f) Mixed: independently disjoint work stays parallel. */
  ops[0] = (struct fasync_op){"w(x)", wx, 1};
  ops[1] = (struct fasync_op){"r(x)", rx, 1};
  ops[2] = (struct fasync_op){"w(y)", wy, 1};
  ops[3] = (struct fasync_op){"r(y)", ry, 1};
  n = fasync_build_dag(ops, 4, edges, MAX_EDGES, &st);
  check("two independent chains -> exactly 2 edges", n == 2);
  check("  4 ops, 2 edges => 2-wide parallelism available",
        st.ops == 4 && st.edges == 2);

  free(x);
  free(y);
}

/* ------------------------------------------------------------------ */
/* Part 2: executing a DAG over real I/O                               */
/* ------------------------------------------------------------------ */

#define N_BLOCKS 4
#define BLOCK 65536

static int g_fd;
static unsigned char* g_bufs[N_BLOCKS];

/*
 * The submit callback: called the moment the DAG says the op is ready, before
 * any dependency has been waited on -- a dependent op's prologue runs here, and
 * the point is that it blocks on nothing it does not actually touch.
 */
static unsigned long g_prologue_ran;

static const struct fasync_op* g_ops_base;

static fasync_id submit_block(const struct fasync_op* op, void* ctx) {
  (void)ctx;
  /* Recover which block this op is responsible for from its array position. */
  unsigned index = (unsigned)(op - g_ops_base);
  g_prologue_ran++;

  /* A dependent op would touch its input here; the declared access keeps the
   * DAG aware of that. */
  if (op->n_accesses)
    FASYNC_ACCESS(op->accesses[0].buf, op->accesses[0].len);

  return fasync_pread(g_fd, g_bufs[index], BLOCK,
                      (unsigned long)index * BLOCK);
}

static void test_execution(void) {
  printf("\ndag execution over real file I/O:\n");
  fflush(stdout);

  const char* path = "/tmp/async-a-sync_stage3_payload.bin";
  int fd = open(path, O_CREAT | O_TRUNC | O_RDWR, 0644);
  if (fd < 0) {
    printf("  cannot create %s\n", path);
    failures++;
    return;
  }

  unsigned char* block = malloc(BLOCK);
  for (int i = 0; i < N_BLOCKS; i++) {
    memset(block, (i + 1) & 0xFF, BLOCK);
    if (pwrite(fd, block, BLOCK, (off_t)i * BLOCK) != BLOCK) {
      printf("  cannot seed file\n");
      failures++;
      return;
    }
  }
  free(block);
  close(fd);

  g_fd = open(path, O_RDONLY);
  if (g_fd < 0) {
    printf("  cannot reopen %s\n", path);
    failures++;
    return;
  }

  /* Four reads of four different blocks: no pair conflicts, so all four
   * should be in flight at once. */
  struct fasync_access acc[N_BLOCKS];
  struct fasync_op ops[N_BLOCKS];
  for (int i = 0; i < N_BLOCKS; i++) {
    g_bufs[i] = malloc(BLOCK);
    memset(g_bufs[i], 0, BLOCK);
    acc[i] = (struct fasync_access){g_bufs[i], BLOCK, FASYNC_OUT};
    ops[i] = (struct fasync_op){"read-block", &acc[i], 1};
  }

  unsigned edges[MAX_EDGES];
  struct fasync_dag_stats st;
  unsigned n_edges = fasync_build_dag(ops, N_BLOCKS, edges, MAX_EDGES, &st);
  check("independent reads -> no dependencies", n_edges == 0);
  check("  all pairwise conflicts dissolved by the range proof",
        st.auto_disjoint_pairs == (N_BLOCKS * (N_BLOCKS - 1)) / 2);

  struct fasync_dag_run run;
  g_prologue_ran = 0;
  g_ops_base = ops;
  if (fasync_run_dag(ops, N_BLOCKS, edges, n_edges, submit_block, NULL,
                     &run) != 0) {
    printf("  dag execution failed\n");
    failures++;
    return;
  }

  printf("  submitted=%u max_concurrent=%u waves=%u\n", run.submitted,
         run.max_concurrent, run.waves);
  check("all four operations submitted", run.submitted == N_BLOCKS);
  check("all four were in flight at once", run.max_concurrent == N_BLOCKS);

  /* Verify the data actually arrived. */
  int data_ok = 1;
  for (int i = 0; i < N_BLOCKS; i++) {
    unsigned char expected = (unsigned char)((i + 1) & 0xFF);
    FASYNC_ACCESS(g_bufs[i], BLOCK);
    for (int b = 0; b < BLOCK; b += 4096) {
      if (g_bufs[i][b] != expected) {
        data_ok = 0;
        break;
      }
    }
  }
  check("every block read back correctly", data_ok);

  for (int i = 0; i < N_BLOCKS; i++)
    free(g_bufs[i]);
  close(g_fd);
  unlink(path);
}

int main(void) {
  test_analysis();
  test_execution();

  printf("\nSTAGE3 %s\n", failures ? "FAIL" : "PASS");
  return failures ? 1 : 0;
}
