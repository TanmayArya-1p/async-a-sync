/*
 * demo_async_io.hh -- three short scenarios, each showing the runtime doing
 * unimpeded work between issue and use (no submit call appears; the queue
 * reaches the kernel by itself):
 *
 *   1. lazy resolution   - queue every read, then read the blocks back
 *   2. blocking vs async - the same reads, waited-on one at a time and all
 *                          issued first
 *   3. dependency DAG    - four operations, conflicting kinds dissolved by
 *                          proved-disjoint ranges
 *
 * The run file (demo_async_io.c) seeds one pattern file and opens it; the
 * scenarios get the open fd and do the reads.
 */
#pragma once

#include <stdio.h>
#include <stdlib.h>

#include "fasync.h"
#include "fasync_dep.h"
#include "utils.hh"

/* 1. Lazy resolution: queue a read per block, then read the blocks back. */
static int scenario_lazy(int fd, int blocks, size_t block_size) {
  unsigned char** bufs = malloc((size_t)blocks * sizeof(unsigned char*));
  fasync_id* ids = malloc((size_t)blocks * sizeof(fasync_id));
  if (!bufs || !ids)
    return 1;
  for (int i = 0; i < blocks; i++)
    bufs[i] = malloc(block_size);
  if (!bufs[0])
    return 1;

  /* Queue a read per block; none of them have been touched. */
  for (int i = 0; i < blocks; i++)
    ids[i] = fasync_pread(fd, bufs[i], block_size, (unsigned long)i * block_size);
  demo_show_submit("queued");

  /* Read the blocks back; the first byte touched is where resolution happens. */
  int ok = 1;
  for (int i = 0; i < blocks; i++)
    if (!ids[i] || fasync_result(ids[i]) != (long)block_size ||
        !demo_block_ok(bufs[i], block_size, i))
      ok = 0;
  demo_show_submit("resolved");
  printf("  %s\n", ok ? "blocks readable, contents correct" : "MISMATCH");

  for (int i = 0; i < blocks; i++)
    free(bufs[i]);
  free(bufs);
  free(ids);
  return ok ? 0 : 1;
}

/* 2. Blocking vs async: the same reads, waited-on per read, or all at once. */
static int scenario_throughput(int fd, int blocks, size_t block_size) {
  unsigned char** bufs = malloc((size_t)blocks * sizeof(unsigned char*));
  fasync_id* ids = malloc((size_t)blocks * sizeof(fasync_id));
  if (!bufs || !ids)
    return 1;
  for (int i = 0; i < blocks; i++)
    bufs[i] = malloc(block_size);

  int ok = 1;

  /* One pread at a time: the program stops for each round trip. */
  demo_start();
  for (int i = 0; i < blocks; i++)
    if (pread(fd, bufs[i], block_size, (off_t)i * block_size) != (ssize_t)block_size)
      ok = 0;
  double blocking_ms = demo_elapsed();

  /* The same reads, all issued before any is consumed. */
  fasync_reset_stats();
  demo_start();
  for (int i = 0; i < blocks; i++)
    ids[i] = fasync_pread(fd, bufs[i], block_size, (unsigned long)i * block_size);
  for (int i = 0; i < blocks; i++)
    fasync_result(ids[i]);
  double async_ms = demo_elapsed();
  for (int i = 0; i < blocks; i++)
    if (!demo_block_ok(bufs[i], block_size, i))
      ok = 0;

  printf("  blocking %7.2f ms   issuing-all %6.2f ms   %s\n", blocking_ms,
         async_ms, ok ? "verified" : "MISMATCH");
  /* Warm cache again: both sides are memcpy-bound here; the win this design
   * is after is removing the per-request round trip, which is what cold-cache
   * runs (and the wordcount demo) show. */

  for (int i = 0; i < blocks; i++)
    free(bufs[i]);
  free(bufs);
  free(ids);
  return ok ? 0 : 1;
}

/* 3. Declared effect sets: the dependency DAG and the ranges it proves apart. */
static int scenario_dependencies(void) {
  void* a = malloc(4096);
  void* b = malloc(4096);
  void* c = malloc(4096);

  struct fasync_access acc0[] = {{a, 1024, FASYNC_OUT}};
  struct fasync_access acc1[] = {{b, 1024, FASYNC_OUT}};
  struct fasync_access acc2[] = {{a, 1024, FASYNC_IN}, {c, 1024, FASYNC_OUT}};
  struct fasync_access acc3[] = {{c, 1024, FASYNC_IN}};

  struct fasync_op ops[4] = {
      {"write(a)", acc0, 1},
      {"write(b)", acc1, 1},
      {"copy(a,c)", acc2, 2},
      {"read(c)", acc3, 1},
  };

  /* Build the dependency DAG from those declarations. */
  unsigned edges[64];
  struct fasync_dag_stats st;
  unsigned n = fasync_build_dag(ops, 4, edges, 64, &st);

  printf("  %u ops, %u edges, %u pairs proven disjoint\n", 4U, n,
         st.auto_disjoint_pairs);
  for (unsigned e = 0; e < n; e++) {
    unsigned from = edges[e] / 4, to = edges[e] % 4;
    printf("    %s -> %s\n", ops[from].name, ops[to].name);
  }

  free(a);
  free(b);
  free(c);
  return 0;
}