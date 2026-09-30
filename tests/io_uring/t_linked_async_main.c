/* A declaration-only annotation links the ordinary implementation in another
 * translation unit, plus generated metadata and runtime forwarders. The
 * runtime runs that implementation once, through this unit's run thunk,
 * before it issues the read. */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "fasync.h"
#include "filc_async.h"

extern volatile int linked_body_calls;

#pragma clang attribute push(__attribute__((annotate("filc_async", "runtime=io_uring", "op=pread", "bout=buf"))), apply_to=function)
void* linked_pread(int fd, void* buf, size_t len, unsigned long offset);
#pragma clang attribute pop

int main(int argc, char** argv)
{
    const char* dir = argc > 1 ? argv[1] : "/tmp";
    char path[512];
    if (snprintf(path, sizeof path, "%s/t_linked_async_%ld.dat",
                 dir, (long)getpid()) >= (int)sizeof path)
        return 1;
    int fd = open(path, O_CREAT | O_TRUNC | O_RDWR, 0600);
    if (fd < 0)
        return 2;
    if (write(fd, "linked", 6) != 6)
        return 3;

    char* buf = malloc(16);
    if (!buf)
        return 4;
    memset(buf, 0, 16);
    struct filc_async_result_s r = { 0 };
    r.pending = linked_pread(fd, buf, 6, 0);
    if (!filc_async_poll(&r))
        filc_async_wait(&r);

    filc_async_stats stats;
    filc_async_get_stats(&stats);
    struct fasync_stats uring;
    fasync_get_stats(&uring);
    int ok = r.state == 0 && r.result == 6 && strcmp(buf, "linked") == 0 &&
             linked_body_calls == 1 && stats.tasks_submitted == 1 &&
             stats.tasks_completed == 1 && uring.sqes_queued == 1;
    free(buf);
    close(fd);
    unlink(path);
    printf("T_LINKED_ASYNC %s (tasks=%lu sqes=%lu body calls=%d)\n",
           ok ? "PASS" : "FAIL", stats.tasks_completed, uring.sqes_queued,
           linked_body_calls);
    return ok ? 0 : 5;
}
