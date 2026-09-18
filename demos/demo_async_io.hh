#pragma once

#include <stdio.h>
#include <stdlib.h>

#include "fasync.h"
#include "fasync_dep.h"
#include "utils.hh"

static int scenario_lazy(int fd, int blocks, size_t block_size) {
     char** bufs = malloc((size_t)blocks * sizeof( char*));
    fasync_id* ids = malloc((size_t)blocks * sizeof(fasync_id));
    for (int i = 0; i < blocks; i++) {
        bufs[i] = malloc(block_size);
    }


    for (int i = 0; i < blocks; i++) {
        ids[i] = fasync_pread(fd, bufs[i], block_size, ( long)i * block_size);
    }

    demo_show_submit("queued");

    // reading blocks back. resolution happens when first byte is touched
    int ok = 1;
    for (int i = 0; i < blocks; i++) {
        if (!ids[i] || fasync_result(ids[i]) != (long)block_size || !demo_block_ok(bufs[i], block_size, i)) {
            ok = 0;
        }
    }

    demo_show_submit("resolved");
    printf("%s\n", ok ? "blocks readable, contents correct" : "MISMATCH");

    for (int i = 0; i < blocks; i++) {
        free(bufs[i]);
    }

    free(bufs);
    free(ids);
    return ok ? 0 : 1;
}

static int scenario_throughput(int fd, int blocks, size_t block_size) {

    char** bufs = malloc((size_t)blocks * sizeof( char*));
    fasync_id* ids = malloc((size_t)blocks * sizeof(fasync_id));
    if (!bufs || !ids) {
    return 1;
    }
    for (int i = 0; i < blocks; i++) {
    bufs[i] = malloc(block_size);
    }

    int ok = 1;

    demo_start();
    for (int i = 0; i < blocks; i++) {
        if (pread(fd, bufs[i], block_size ,(off_t)i * block_size) != (ssize_t)block_size) {
            ok = 0;
        }
    }

    double blocking_ms = demo_elapsed();

    fasync_reset_stats();
    demo_start();

    for (int i = 0; i < blocks; i++) {
        ids[i] = fasync_pread(fd, bufs[i], block_size, ( long)i * block_size);
    }
    for (int i = 0; i < blocks; i++) {
        fasync_result(ids[i]);
    }

    double async_ms = demo_elapsed();
    for (int i = 0; i < blocks; i++) {
        if (!demo_block_ok(bufs[i], block_size, i)) {
            ok = 0;
        }
    }


  printf("  blocking %7.2f ms   issuing-all %6.2f ms   %s\n", blocking_ms,
         async_ms, ok ? "verified" : "MISMATCH");

  for (int i = 0; i < blocks; i++) {
    free(bufs[i]);
  }
  free(bufs);
  free(ids);
  return ok ? 0 : 1;
}


// builds depedency dag and executes them
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

    unsigned edges[64];
    struct fasync_dag_stats st;
    unsigned n = fasync_build_dag(ops, 4, edges, 64, &st);

    printf("  %u ops, %u edges, %u pairs proven disjoint\n", 4U, n, st.auto_disjoint_pairs);
    for (unsigned e = 0; e < n; e++) {
        size_t from = edges[e] / 4, to = edges[e] % 4;
        printf("    %s -> %s\n", ops[from].name, ops[to].name);
    }

    free(a);
    free(b);
    free(c);
    return 0;
}
