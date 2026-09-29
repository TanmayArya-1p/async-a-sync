/* tests/t_threads.c -- annotated calls from two threads.
 *
 * Each thread issues annotated reads into buffers of its own, with no wait,
 * then both meet and each reads the other's buffers. Every read goes through
 * the one io_uring ring and the framework's shared state, and a buffer still
 * pending when the other thread touches it must be waited for, not read as
 * it stands. */
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "filc_async.h"

#define READS 64
#define LEN 16

#pragma clang attribute push(__attribute__((annotate("filc_async", "op=pread", "fd=0", "bout=1"))), apply_to=function)
void* async_pread(int fd, void* buf, size_t len, unsigned long offset)
{
    (void)fd;
    (void)buf;
    (void)len;
    (void)offset;
    return 0;
}
#pragma clang attribute pop

static int g_fd;
static char* g_bufs[2][READS];
static pthread_barrier_t g_issued;
static int g_bad[2];

static char expected(int i)
{
    return (char)('a' + i % 26);
}

static void* worker(void* arg)
{
    int me = (int)(long)arg;
    int other = 1 - me;
    for (int i = 0; i < READS; i++) {
        g_bufs[me][i] = calloc(1, LEN);
        async_pread(g_fd, g_bufs[me][i], LEN, (unsigned long)i * LEN);
    }
    pthread_barrier_wait(&g_issued);
    /* the other thread's reads may still be in flight */
    for (int i = 0; i < READS; i++)
        for (int j = 0; j < LEN; j++)
            g_bad[me] += g_bufs[other][i][j] != expected(i);
    return NULL;
}

int main(int argc, char** argv)
{
    const char* dir = argc > 1 ? argv[1] : "/tmp";
    char path[512];
    snprintf(path, sizeof path, "%s/t_threads_%ld.dat", dir, (long)getpid());
    g_fd = open(path, O_CREAT | O_TRUNC | O_RDWR, 0600);
    if (g_fd < 0)
        return 1;
    char block[LEN];
    for (int i = 0; i < READS; i++) {
        memset(block, expected(i), LEN);
        if (pwrite(g_fd, block, LEN, (off_t)i * LEN) != LEN)
            return 1;
    }

    pthread_barrier_init(&g_issued, NULL, 2);
    pthread_t t[2];
    for (long k = 0; k < 2; k++)
        if (pthread_create(&t[k], NULL, worker, (void*)k) != 0)
            return 1;
    for (int k = 0; k < 2; k++)
        pthread_join(t[k], NULL);

    filc_async_stats stats;
    filc_async_get_stats(&stats);
    int ok = !g_bad[0] && !g_bad[1] && stats.tasks_submitted == 2 * READS &&
             stats.tasks_completed == 2 * READS && stats.tasks_failed == 0;
    close(g_fd);
    unlink(path);
    printf("T_THREADS %s (bad=%d/%d submitted=%lu completed=%lu)\n",
           ok ? "PASS" : "FAIL", g_bad[0], g_bad[1], stats.tasks_submitted,
           stats.tasks_completed);
    return ok ? 0 : 1;
}
