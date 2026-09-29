/* tests/t_pragma_lazy_many.c -- annotated calls that are never polled or
 * waited on.
 *
 * The runtime has 1024 request slots and 1024 pending marks. A program that
 * only resolves its buffers by touching them never delivers a completion
 * through poll/wait, so those resources have to come back some other way, or
 * the 1025th call dies ("too many pending buffers") or fails with -EAGAIN.
 *
 *   1. CALLS lazy reads, each checked through the compiler's access hook
 *      (a bout= mark and a request slot per call);
 *   2. CALLS unordered writes, never waited on (a request slot per call and
 *      no marks), then the file is read back.
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "fasync.h"
#include "filc_async.h"

#define CALLS 3000

#pragma clang attribute push(__attribute__((annotate("filc_async", "runtime=io_uring", "op=pread", "bout=1"))), apply_to=function)
void* lazy_pread(int fd, void* buf, size_t len, unsigned long offset)
{
    (void)fd;
    (void)buf;
    (void)len;
    (void)offset;
    return 0;
}
#pragma clang attribute pop

#pragma clang attribute push(__attribute__((annotate("filc_async", "runtime=io_uring", "op=pwrite", "bin=1"))), apply_to=function)
void* lazy_pwrite(int fd, const void* buf, size_t len, unsigned long offset)
{
    (void)fd;
    (void)buf;
    (void)len;
    (void)offset;
    return 0;
}
#pragma clang attribute pop

int main(int argc, char** argv)
{
    const char* dir = argc > 1 ? argv[1] : "/tmp";
    char path[512];
    if (snprintf(path, sizeof path, "%s/t_pragma_lazy_many_%ld.dat",
                 dir, (long)getpid()) >= (int)sizeof path)
        return 1;
    int fd = open(path, O_CREAT | O_TRUNC | O_RDWR, 0600);
    if (fd < 0 || write(fd, "lazymany", 8) != 8) {
        perror("t_pragma_lazy_many: setup");
        return 1;
    }

    int bad_reads = 0;
    for (int i = 0; i < CALLS; i++) {
        char* buf = malloc(8);
        if (!buf)
            return 1;
        memset(buf, 0, 8);
        lazy_pread(fd, buf, 8, 0);
        int ok = 1;
        for (int j = 0; j < 8; j++)
            ok &= buf[j] == "lazymany"[j];
        bad_reads += !ok;
        free(buf);
    }

    static const char byte = 'w';
    for (int i = 0; i < CALLS; i++)
        lazy_pwrite(fd, &byte, 1, (unsigned long)(8 + i));

    /* Let every request finish, then have the runtime observe them. */
    fasync_wait_all();
    filc_async_stats stats;
    filc_async_get_stats(&stats);

    int bad_writes = 0;
    char* back = malloc(CALLS);
    if (!back || pread(fd, back, CALLS, 8) != CALLS)
        bad_writes = CALLS;
    else
        for (int i = 0; i < CALLS; i++)
            bad_writes += back[i] != 'w';
    free(back);
    close(fd);
    unlink(path);

    int ok = !bad_reads && !bad_writes &&
             stats.tasks_submitted == 2 * CALLS &&
             stats.tasks_completed == 2 * CALLS && stats.tasks_failed == 0;
    printf("T_PRAGMA_LAZY_MANY %s (bad_reads=%d bad_writes=%d submitted=%lu "
           "completed=%lu failed=%lu)\n",
           ok ? "PASS" : "FAIL", bad_reads, bad_writes, stats.tasks_submitted,
           stats.tasks_completed, stats.tasks_failed);
    return ok ? 0 : 1;
}
