/* tests/t_pragma_reuse_lazy.c -- reusing buffers whose reads were only
 * resolved by touching them must not flush the submission queue.
 *
 * A task whose buffer the compiler's hook resolved stays in flight for
 * filc_async until something observes it, so its pending mark stays. The
 * next annotated call on that buffer retires the stale task first. The task's
 * request is already finished, so that must not submit anything: the new
 * calls should still queue and reach the kernel in one entry when a buffer is
 * first read, as the first pass did. Flushing there sends each earlier call's
 * request separately, one kernel entry per call.
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "fasync.h"
#include "filc_async.h"

#define BUFS 8
#define LEN 16

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

static char* bufs[BUFS];

/* Issues one read per buffer, then touches each. Returns the kernel submit
 * entries made while issuing, or -1 when a buffer holds the wrong bytes. */
static long pass(int fd)
{
    for (int i = 0; i < BUFS; i++)
        memset(bufs[i], 0, LEN);
    fasync_reset_stats();
    for (int i = 0; i < BUFS; i++)
        lazy_pread(fd, bufs[i], LEN, (unsigned long)i * LEN);
    struct fasync_stats issued;
    fasync_get_stats(&issued);

    for (int i = 0; i < BUFS; i++)
        if (bufs[i][0] != 'a' + i)
            return -1;
    return (long)issued.kernel_submit_entries;
}

int main(int argc, char** argv)
{
    const char* dir = argc > 1 ? argv[1] : "/tmp";
    char path[512];
    if (snprintf(path, sizeof path, "%s/t_pragma_reuse_lazy_%ld.dat", dir,
                 (long)getpid()) >= (int)sizeof path)
        return 1;
    int fd = open(path, O_CREAT | O_TRUNC | O_RDWR, 0600);
    if (fd < 0)
        return 1;
    char block[LEN];
    for (int i = 0; i < BUFS; i++) {
        memset(block, 'a' + i, LEN);
        if (pwrite(fd, block, LEN, (off_t)i * LEN) != LEN)
            return 1;
    }
    for (int i = 0; i < BUFS; i++) {
        bufs[i] = malloc(LEN);
        if (!bufs[i])
            return 1;
    }

    long first = pass(fd);
    long second = pass(fd);

    close(fd);
    unlink(path);
    int ok = first == 0 && second == 0;
    printf("T_PRAGMA_REUSE_LAZY %s (submits while issuing: first pass %ld, "
           "second pass %ld)\n",
           ok ? "PASS" : "FAIL", first, second);
    return ok ? 0 : 1;
}
