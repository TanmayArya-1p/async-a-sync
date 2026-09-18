/*
 * stage5_trackers.c -- async-a-sync.pdf's serialization token versus declared
 * effect sets: one workload, three encodings, so the comparison is data.
 *
 *   workload: op0,op1 root; op2,op4 use op0's result; op3,op5 use op1's.
 *   precise effect sets   4 edges, four ops in flight
 *   two tokens            6 edges, two in flight
 *   one token             15 edges, fully serialized
 *
 * A token can only express "after everything else on this token", so its price
 * is over-serialization wherever the DAG is wider than a chain.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

#include "fasync.h"
#include "fasync_dep.h"

#define N_OPS 6
#define BLOCK 262144

static int failures = 0;

static void check(const char* what, int ok) {
  printf("  %-56s %s\n", what, ok ? "ok" : "FAIL");
  fflush(stdout);
  if (!ok)
    failures++;
}

/* ------------------------------------------------------------------ */
/* Real I/O behind the DAG                                             */
/* ------------------------------------------------------------------ */

static int g_fd;
static unsigned char* g_bufs[N_OPS];
static const struct fasync_op* g_ops_base;

static fasync_id submit_op(const struct fasync_op* op, void* ctx) {
  (void)ctx;
  unsigned index = (unsigned)(op - g_ops_base);
  return fasync_pread(g_fd, g_bufs[index], BLOCK, (unsigned long)index * BLOCK);
}

/* Run a DAG over the six reads and report the peak concurrency reached. */
static unsigned run_and_measure(const struct fasync_op* ops, unsigned n_edges,
                                const unsigned* edges, unsigned* out_edges) {
  struct fasync_dag_run run;
  memset(&run, 0, sizeof(run));
  g_ops_base = ops;
  if (fasync_run_dag(ops, N_OPS, edges, n_edges, submit_op, NULL, &run) != 0)
    return 0;
  if (out_edges)
    *out_edges = n_edges;
  return run.max_concurrent;
}

int main(void) {
  const char* path = "/tmp/async-a-sync_stage5_payload.bin";

  /* Seed six distinguishable blocks. */
  int fd = open(path, O_CREAT | O_TRUNC | O_RDWR, 0644);
  if (fd < 0) {
    printf("cannot create %s\n", path);
    return 1;
  }
  unsigned char* seed = malloc(BLOCK);
  for (int i = 0; i < N_OPS; i++) {
    memset(seed, (i + 1) & 0xFF, BLOCK);
    if (pwrite(fd, seed, BLOCK, (off_t)i * BLOCK) != BLOCK) {
      printf("cannot seed\n");
      return 1;
    }
  }
  free(seed);
  close(fd);
  g_fd = open(path, O_RDONLY);
  if (g_fd < 0)
    return 1;

  for (int i = 0; i < N_OPS; i++)
    g_bufs[i] = malloc(BLOCK);

  unsigned edges[64];

  /* ---------------------------------------------------------------- */
  /* Encoding 1: precise effect sets.                                  */
  /* ---------------------------------------------------------------- */
  printf("encoding 1: declared effect sets (in/out over buffer ranges)\n");

  struct fasync_access precise[N_OPS];
  struct fasync_op ops_precise[N_OPS];
  precise[0] = FASYNC_ACCESS_RANGE(g_bufs[0], BLOCK, FASYNC_OUT);
  precise[1] = FASYNC_ACCESS_RANGE(g_bufs[1], BLOCK, FASYNC_OUT);
  /* op2 and op4 consume op0's result; op3 and op5 consume op1's. */
  struct fasync_access acc2[2], acc3[2], acc4[2], acc5[2];
  acc2[0] = FASYNC_ACCESS_RANGE(g_bufs[0], BLOCK, FASYNC_IN);
  acc2[1] = FASYNC_ACCESS_RANGE(g_bufs[2], BLOCK, FASYNC_OUT);
  acc3[0] = FASYNC_ACCESS_RANGE(g_bufs[1], BLOCK, FASYNC_IN);
  acc3[1] = FASYNC_ACCESS_RANGE(g_bufs[3], BLOCK, FASYNC_OUT);
  acc4[0] = FASYNC_ACCESS_RANGE(g_bufs[0], BLOCK, FASYNC_IN);
  acc4[1] = FASYNC_ACCESS_RANGE(g_bufs[4], BLOCK, FASYNC_OUT);
  acc5[0] = FASYNC_ACCESS_RANGE(g_bufs[1], BLOCK, FASYNC_IN);
  acc5[1] = FASYNC_ACCESS_RANGE(g_bufs[5], BLOCK, FASYNC_OUT);

  ops_precise[0] = (struct fasync_op){"read0", &precise[0], 1};
  ops_precise[1] = (struct fasync_op){"read1", &precise[1], 1};
  ops_precise[2] = (struct fasync_op){"read2(after0)", acc2, 2};
  ops_precise[3] = (struct fasync_op){"read3(after1)", acc3, 2};
  ops_precise[4] = (struct fasync_op){"read4(after0)", acc4, 2};
  ops_precise[5] = (struct fasync_op){"read5(after1)", acc5, 2};

  struct fasync_dag_stats st;
  unsigned n_precise = fasync_build_dag(ops_precise, N_OPS, edges, 64, &st);
  printf("  edges: %u   (write->read: %u, read->write: %u, write->write: %u)\n",
         n_precise, st.read_write_edges, st.write_read_edges,
         st.write_write_edges);
  check("four edges: one per real dependency", n_precise == 4);

  /* ---------------------------------------------------------------- */
  /* Encoding 2: one token per independent chain.                      */
  /* ---------------------------------------------------------------- */
  printf("\nencoding 2: two tokens, one per independent chain\n");

  fasync_tracker* t0 = fasync_tracker_new();
  fasync_tracker* t1 = fasync_tracker_new();

  struct fasync_access tok_chain0[4], tok_chain1[4], tok[2];
  tok[0] = fasync_tracker_access(t0, FASYNC_INOUT);
  tok[1] = fasync_tracker_access(t1, FASYNC_INOUT);
  for (int i = 0; i < 4; i++) {
    tok_chain0[i] = tok[0];
    tok_chain1[i] = tok[1];
  }

  struct fasync_op ops_tokens[N_OPS];
  ops_tokens[0] = (struct fasync_op){"chain0", &tok_chain0[0], 1};
  ops_tokens[1] = (struct fasync_op){"chain1", &tok_chain1[0], 1};
  ops_tokens[2] = (struct fasync_op){"chain0", &tok_chain0[1], 1};
  ops_tokens[3] = (struct fasync_op){"chain1", &tok_chain1[1], 1};
  ops_tokens[4] = (struct fasync_op){"chain0", &tok_chain0[2], 1};
  ops_tokens[5] = (struct fasync_op){"chain1", &tok_chain1[2], 1};

  unsigned n_tokens = fasync_build_dag(ops_tokens, N_OPS, edges, 64, &st);
  printf("  edges: %u\n", n_tokens);
  check("a token can only chain, so each 3-op chain is fully connected",
        n_tokens == 6);

  /* ---------------------------------------------------------------- */
  /* Encoding 3: one token for the whole workload (the PDF's example). */
  /* ---------------------------------------------------------------- */
  printf("\nencoding 3: a single token shared by every call\n");

  fasync_tracker* all = fasync_tracker_new();
  struct fasync_access tok_all[4];
  for (int i = 0; i < 4; i++)
    tok_all[i] = fasync_tracker_access(all, FASYNC_INOUT);

  struct fasync_op ops_one[N_OPS];
  for (int i = 0; i < N_OPS; i++)
    ops_one[i] = (struct fasync_op){"all", &tok_all[i % 4], 1};

  unsigned n_one = fasync_build_dag(ops_one, N_OPS, edges, 64, &st);
  printf("  edges: %u  (every pair, i.e. a total order)\n", n_one);
  check("one token => completely serialized", n_one == (N_OPS * (N_OPS - 1)) / 2);

  /* ---------------------------------------------------------------- */
  /* Execution: what each encoding actually achieves.                  */
  /* ---------------------------------------------------------------- */
  printf("\nexecution over real I/O (peak operations in flight):\n");

  unsigned measured_precise = run_and_measure(ops_precise, n_precise, edges, 0);
  printf("  precise effect sets:  max_concurrent = %u\n", measured_precise);

  /* Rebuild the token DAGs: run_and_measure consumes the edge list. */
  n_tokens = fasync_build_dag(ops_tokens, N_OPS, edges, 64, 0);
  unsigned measured_tokens = run_and_measure(ops_tokens, n_tokens, edges, 0);
  printf("  two tokens:           max_concurrent = %u\n", measured_tokens);

  n_one = fasync_build_dag(ops_one, N_OPS, edges, 64, 0);
  unsigned measured_one = run_and_measure(ops_one, n_one, edges, 0);
  printf("  one token:            max_concurrent = %u\n", measured_one);

  check("precise effect sets expose the full width of the workload",
        measured_precise >= 4);
  check("tokens expose less parallelism than the precise encoding",
        measured_tokens < measured_precise);
  check("a single shared token serializes everything", measured_one == 1);

  /* ---------------------------------------------------------------- */
  /* Where a token is genuinely better.                                */
  /* -------------------------------------------------- */
  printf("\nwhere a token is genuinely better:\n");

  /*
   * (a) An opaque dependency with no address: collisions on an fd, path, or
   * device have nothing the capability-range analysis can look at.
   */
  fasync_tracker* fd_tok = fasync_tracker_new();
  struct fasync_access fd_acc[2];
  fd_acc[0] = fasync_tracker_access(fd_tok, FASYNC_INOUT);
  fd_acc[1] = fasync_tracker_access(fd_tok, FASYNC_INOUT);
  struct fasync_op fd_ops[2] = {{"same-fd", &fd_acc[0], 1},
                                {"same-fd", &fd_acc[1], 1}};
  unsigned n_fd = fasync_build_dag(fd_ops, 2, edges, 64, 0);
  check("a token expresses a dependency on a non-address resource", n_fd == 1);

  /*
   * (b) A token read IN rather than INOUT: any number of readers share one at no
   * cost -- something the PDF's always-serializing token cannot express.
   */
  fasync_tracker* reader_tok = fasync_tracker_new();
  struct fasync_access rd[3];
  rd[0] = fasync_tracker_access(reader_tok, FASYNC_IN);
  rd[1] = fasync_tracker_access(reader_tok, FASYNC_IN);
  rd[2] = fasync_tracker_access(reader_tok, FASYNC_IN);
  struct fasync_op rd_ops[3] = {{"reader", &rd[0], 1},
                                {"reader", &rd[1], 1},
                                {"reader", &rd[2], 1}};
  unsigned n_rd = fasync_build_dag(rd_ops, 3, edges, 64, 0);
  check("sharing a token as a reader costs no edge (PDF model cannot do this)",
        n_rd == 0);

  /* Clean up. */
  fasync_tracker_free(t0);
  fasync_tracker_free(t1);
  fasync_tracker_free(all);
  fasync_tracker_free(fd_tok);
  fasync_tracker_free(reader_tok);
  for (int i = 0; i < N_OPS; i++)
    free(g_bufs[i]);
  close(g_fd);
  unlink(path);

  printf("\nSTAGE5 %s\n", failures ? "FAIL" : "PASS");
  return failures ? 1 : 0;
}
