/* Declaration and definition dependencies order conflicting requests. */
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "fasync.h"
#include "filc_async.h"

static volatile int body_calls;

__attribute__((noinline)) void* dep_write(int fd, const void* buf, size_t len,
                                           unsigned long offset);

#pragma clang attribute push(__attribute__((annotate("filc_async", "runtime=io_uring", "op=pread", "bout=buf", "r_dep=fd:file", "w_dep=buf:mem"))), apply_to=function)
__attribute__((noinline)) void* dep_read(int fd, void* buf, size_t len,
                                          unsigned long offset);
#pragma clang attribute pop

#pragma clang attribute push(__attribute__((annotate("filc_async", "runtime=io_uring", "op=pwrite", "bin=buf", "w_dep=fd:file"))), apply_to=function)
void* dep_write(int fd, const void* buf, size_t len, unsigned long offset)
{
    body_calls++;
    return (void*)(long)(fd + (buf != NULL) + len + offset);
}
#pragma clang attribute pop

void* dep_read(int fd, void* buf, size_t len, unsigned long offset)
{
    body_calls++;
    return (void*)(long)(fd + (buf != NULL) + len + offset);
}

static long finish(void* task)
{
    struct filc_async_result_s result = { .pending = task };
    filc_async_wait(&result);
    return result.state == 0 ? result.result : -1;
}

int main(int argc, char** argv)
{
    const char* dir = argc > 1 ? argv[1] : "/tmp";
    char path[512];
    if (snprintf(path, sizeof path, "%s/t_pragma_dependencies_%ld.dat",
                 dir, (long)getpid()) >= (int)sizeof path)
        return 1;
    int fd = open(path, O_CREAT | O_TRUNC | O_RDWR, 0600);
    if (fd < 0)
        return 2;

    char buf[8] = { 0 };
    void* first = dep_write(fd, "first", 5, 0);
    void* second = dep_write(fd, "last!", 5, 0);
    void* third = dep_read(fd, buf, 5, 0);
    filc_async_stats before;
    filc_async_get_stats(&before);
    if (before.tasks_submitted != 3)
        return 3;

    long nr = finish(third);
    long nw1 = finish(first);
    long nw2 = finish(second);
    /* Each call's body ran once, in the runtime, and each became one
     * request; the later calls waited for the locks on fd. */
    filc_async_stats after;
    filc_async_get_stats(&after);
    struct fasync_stats uring;
    fasync_get_stats(&uring);
    int ok = nr == 5 && nw1 == 5 && nw2 == 5 &&
             strcmp(buf, "last!") == 0 && body_calls == 3 &&
             after.tasks_completed == 3 && after.tasks_failed == 0 &&
             after.lock_waits == 2 && uring.sqes_queued == 3;
    close(fd);
    unlink(path);
    printf("T_PRAGMA_DEPENDENCIES %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 4;
}
