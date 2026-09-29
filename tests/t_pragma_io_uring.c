/* End-to-end annotated syscall dispatch through the io_uring backend. */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "fasync.h"
#include "filc_async.h"

static volatile int body_calls;

#pragma clang attribute push(__attribute__((annotate("filc_async", "op=openat", "fd=0", "bin=1"))), apply_to=function)
__attribute__((noinline)) void* async_openat(int dirfd, const char* path, int flags, int mode)
{
    body_calls++;
    return (void*)(long)(dirfd + flags + mode + (path != NULL));
}
#pragma clang attribute pop

#pragma clang attribute push(__attribute__((annotate("filc_async", "op=pread", "fd=0", "bout=1", "r_dep=0", "w_dep=1"))), apply_to=function)
__attribute__((noinline)) void* async_pread(int fd, void* buf, size_t len, unsigned long offset)
{
    body_calls++;
    return (void*)(long)(fd + (buf != NULL) + len + offset);
}
#pragma clang attribute pop

#pragma clang attribute push(__attribute__((annotate("filc_async", "op=pwrite", "fd=0", "bin=1", "w_dep=0"))), apply_to=function)
__attribute__((noinline)) void* async_pwrite(int fd, const void* buf, size_t len, unsigned long offset)
{
    body_calls++;
    return (void*)(long)(fd + (buf != NULL) + len + offset);
}
#pragma clang attribute pop

#pragma clang attribute push(__attribute__((annotate("filc_async", "op=fsync", "fd=0"))), apply_to=function)
__attribute__((noinline)) void* async_fsync(int fd)
{
    body_calls++;
    return (void*)(long)fd;
}
#pragma clang attribute pop

#pragma clang attribute push(__attribute__((annotate("filc_async", "op=close", "fd=0"))), apply_to=function)
__attribute__((noinline)) void* async_close(int fd)
{
    body_calls++;
    return (void*)(long)fd;
}
#pragma clang attribute pop

static long wait_result(void* pending)
{
    struct filc_async_result_s result = { 0 };
    result.pending = pending;
    filc_async_wait(&result);
    return result.result;
}

int main(int argc, char** argv)
{
    const char* dir = argc > 1 ? argv[1] : "/tmp";
    char path[512];
    if (snprintf(path, sizeof path, "%s/t_pragma_io_uring_%ld.dat",
                 dir, (long)getpid()) >= (int)sizeof path)
        return 1;

    int seed_fd = open(path, O_CREAT | O_TRUNC | O_RDWR, 0600);
    if (seed_fd < 0)
        return 2;
    if (write(seed_fd, "original", 8) != 8 || close(seed_fd) != 0)
        return 3;

    int fd = (int)wait_result(async_openat(AT_FDCWD, path, O_RDWR, 0));
    if (fd < 0)
        return 4;

    char* buf = malloc(16);
    if (!buf)
        return 5;
    memset(buf, 0, 16);
    struct filc_async_result_s read_result = { 0 };
    read_result.pending = async_pread(fd, buf, 8, 0);
    if (!filc_async_is_pending(buf))
        return 6;
    /* This load must publish and resolve the queued read without an explicit
     * submit or wait. The later wait retires the result handle. */
    if (buf[0] != 'o')
        return 7;
    filc_async_wait(&read_result);
    if (read_result.result != 8 || strcmp(buf, "original") != 0 ||
        filc_async_is_pending(buf))
        return 8;

    if (wait_result(async_pwrite(fd, "replaced", 8, 0)) != 8)
        return 9;
    if (wait_result(async_fsync(fd)) != 0)
        return 10;
    memset(buf, 0, 16);
    if (wait_result(async_pread(fd, buf, 8, 0)) != 8 ||
        strcmp(buf, "replaced") != 0)
        return 11;
    /* Re-marking the same output must finish its previous owner first. */
    void* first = async_pread(fd, buf, 8, 0);
    void* second = async_pread(fd, buf, 8, 0);
    if (wait_result(first) != 8 || wait_result(second) != 8 ||
        strcmp(buf, "replaced") != 0)
        return 14;
    if (wait_result(async_close(fd)) != 0)
        return 12;

    /* The runtime ran each body once. The second read into buf waited for
     * the first, through buf's write lock or its pending mark. */
    filc_async_stats stats;
    filc_async_get_stats(&stats);
    struct fasync_stats uring;
    fasync_get_stats(&uring);
    int ok = body_calls == 8 && stats.tasks_submitted == 8 &&
             stats.tasks_completed == 8 && stats.tasks_failed == 0 &&
             uring.sqes_queued >= 8 &&
             stats.lock_waits + stats.pending_resolves >= 1;
    free(buf);
    unlink(path);
    printf("T_PRAGMA_IO_URING %s (submitted=%lu completed=%lu sqes=%lu "
           "body calls=%d)\n",
           ok ? "PASS" : "FAIL", stats.tasks_submitted,
           stats.tasks_completed, uring.sqes_queued, body_calls);
    return ok ? 0 : 13;
}
