#pragma once

#include <stdio.h>
#include <stdlib.h>

#include "fasync.h"
#include "fasync_dep.h"
#include "utils.hh"

static unsigned char** demo_alloc_blocks(int blocks, size_t block_size) {
    unsigned char** bufs = calloc((size_t)blocks, sizeof(unsigned char*));
    if (!bufs) {
        return 0;
    }
    for (int i = 0; i < blocks; i++) {
        bufs[i] = calloc(1, block_size);
        if (!bufs[i]) {
            for (int j = 0; j < i; j++) {
                free(bufs[j]);
            }
            free(bufs);
            return 0;
        }
    }
    return bufs;
}

static void demo_free_blocks(unsigned char** bufs, int blocks) {
    for (int i = 0; i < blocks; i++) {
        free(bufs[i]);
    }
    free(bufs);
}

// Issue every read, then touch each buffer. Nothing waits explicitly: the
// first byte a check reads is where the compiler's hook resolves the block.
static int scenario_lazy(int fd, int blocks, size_t block_size) {
    unsigned char** bufs = demo_alloc_blocks(blocks, block_size);
    fasync_id* ids = malloc((size_t)blocks * sizeof(fasync_id));
    if (!bufs || !ids) {
        free(ids);
        if (bufs) {
            demo_free_blocks(bufs, blocks);
        }
        return 1;
    }

    int ok = 1;
    for (int i = 0; i < blocks; i++) {
        ids[i] = fasync_pread(fd, bufs[i], block_size, (unsigned long)i * block_size);
        if (!ids[i]) {
            fprintf(stderr, "fasync_pread block %d: %s\n", i, fasync_last_error());
            ok = 0;
        }
    }

    demo_show_submit("queued");

    for (int i = 0; i < blocks; i++) {
        if (!ids[i]) {
            continue;
        }
        if (!demo_block_ok(bufs[i], block_size, i)) {
            ok = 0;
        }
        // Already resolved by the access above; this only reaps the handle.
        if (fasync_result(ids[i]) != (long)block_size) {
            ok = 0;
        }
    }

    demo_show_submit("resolved");
    printf("%s\n", ok ? "blocks readable, contents correct" : "MISMATCH");

    demo_free_blocks(bufs, blocks);
    free(ids);
    return ok ? 0 : 1;
}

// The same reads twice: blocking one at a time, then all issued up front.
static int scenario_throughput(int fd, int blocks, size_t block_size) {
    unsigned char** bufs = demo_alloc_blocks(blocks, block_size);
    fasync_id* ids = malloc((size_t)blocks * sizeof(fasync_id));
    if (!bufs || !ids) {
        free(ids);
        if (bufs) {
            demo_free_blocks(bufs, blocks);
        }
        return 1;
    }

    int ok = 1;

    demo_start();
    for (int i = 0; i < blocks; i++) {
        if (pread(fd, bufs[i], block_size, (off_t)i * block_size) != (ssize_t)block_size) {
            ok = 0;
        }
    }
    double blocking_ms = demo_elapsed();

    // Clear the buffers so the async arm is verified on its own bytes, not
    // on what the blocking arm left behind.
    for (int i = 0; i < blocks; i++) {
        memset(bufs[i], 0, block_size);
    }

    fasync_reset_stats();
    demo_start();
    for (int i = 0; i < blocks; i++) {
        ids[i] = fasync_pread(fd, bufs[i], block_size, (unsigned long)i * block_size);
    }
    for (int i = 0; i < blocks; i++) {
        if (!ids[i] || fasync_result(ids[i]) != (long)block_size) {
            ok = 0;
        }
    }
    double async_ms = demo_elapsed();

    for (int i = 0; i < blocks; i++) {
        if (!demo_block_ok(bufs[i], block_size, i)) {
            ok = 0;
        }
    }

    printf("  blocking %7.2f ms   issuing-all %6.2f ms   %s\n", blocking_ms,
           async_ms, ok ? "verified" : "MISMATCH");

    demo_free_blocks(bufs, blocks);
    free(ids);
    return ok ? 0 : 1;
}

// Builds the dependency DAG for four ops and prints its edges.
static int scenario_dependencies(void) {
    void* a = malloc(4096);
    void* b = malloc(4096);
    void* c = malloc(4096);
    if (!a || !b || !c) {
        free(a);
        free(b);
        free(c);
        return 1;
    }

    struct fasync_access acc0[] = {FASYNC_ACCESS_RANGE(a, 1024, FASYNC_OUT)};
    struct fasync_access acc1[] = {FASYNC_ACCESS_RANGE(b, 1024, FASYNC_OUT)};
    struct fasync_access acc2[] = {FASYNC_ACCESS_RANGE(a, 1024, FASYNC_IN),
                                   FASYNC_ACCESS_RANGE(c, 1024, FASYNC_OUT)};
    struct fasync_access acc3[] = {FASYNC_ACCESS_RANGE(c, 1024, FASYNC_IN)};

    enum { N_OPS = 4 };
    struct fasync_op ops[N_OPS] = {
        {"write(a)", acc0, 1},
        {"write(b)", acc1, 1},
        {"copy(a,c)", acc2, 2},
        {"read(c)", acc3, 1},
    };

    unsigned edges[N_OPS * N_OPS];
    struct fasync_dag_stats st;
    unsigned n = fasync_build_dag(ops, N_OPS, edges, N_OPS * N_OPS, &st);

    printf("  %u ops, %u edges, %u pairs proven disjoint\n", (unsigned)N_OPS, n,
           st.auto_disjoint_pairs);
    for (unsigned e = 0; e < n; e++) {
        unsigned from = edges[e] / N_OPS;
        unsigned to = edges[e] % N_OPS;
        printf("    %s -> %s\n", ops[from].name, ops[to].name);
    }

    free(a);
    free(b);
    free(c);
    return 0;
}
