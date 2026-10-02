/* The program's own accesses to a buffer a queued annotated call is still
 * reading wait for that call. Built with the patched compiler, whose access
 * hook calls into the framework while the buffer carries the pending flag. */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "filc_async.h"

/* pwrite: buf is a pointer to const, so the call only reads it. */
FILC_ASYNC(io_uring, FILC_OP(pwrite))
void* write_at(int fd, const void* buf, size_t len, long offset)
{
    (void)fd;
    (void)buf;
    (void)len;
    (void)offset;
    return 0;
}

static const char literal[] = "xy";

static int failures;

static void check(const char* what, int ok)
{
    printf("  %-58s %s\n", what, ok ? "ok" : "FAIL");
    failures += !ok;
}

static long wait_call(void* task)
{
    struct filc_async_result_s r = {0};
    r.pending = task;
    filc_async_wait(&r);
    return r.result;
}

static unsigned long hooks(void)
{
    filc_async_stats s;
    filc_async_get_stats(&s);
    return s.hook_resolves;
}

static unsigned long resolves(void)
{
    filc_async_stats s;
    filc_async_get_stats(&s);
    return s.pending_resolves;
}

int main(int argc, char** argv)
{
    char path[256];
    snprintf(path, sizeof path, "%s/t_host_access_after_read.dat",
             argc > 1 ? argv[1] : "/tmp");
    int fd = open(path, O_CREAT | O_TRUNC | O_RDWR, 0644);
    if (fd < 0) {
        perror(path);
        return 1;
    }
    char* msg = malloc(4);

    /* A store into a buffer a queued write is still sending waits for it. */
    msg[0] = 'A';
    void* first = write_at(fd, msg, 1, 0);
    msg[0] = 'B';
    void* second = write_at(fd, msg, 1, 1);
    long r1 = wait_call(first), r2 = wait_call(second);
    char back[3] = {0};
    pread(fd, back, 2, 0);
    check("a store after a queued write waits: the file holds \"AB\"",
          r1 == 1 && r2 == 1 && strcmp(back, "AB") == 0);

    /* So does a read: it sees the bytes the call is sending. */
    msg[0] = 'C';
    void* third = write_at(fd, msg, 1, 2);
    unsigned long hooks_before = hooks();
    char seen = msg[0];
    struct filc_async_result_s r = {0};
    r.pending = third;
    check("a read after a queued write waits for it",
          seen == 'C' && hooks() > hooks_before && filc_async_poll(&r) &&
              r.result == 1);

    /* Two calls reading one buffer do not wait for each other. */
    msg[0] = 'D';
    unsigned long resolves_before = resolves();
    void* fourth = write_at(fd, msg, 1, 3);
    void* fifth = write_at(fd, msg, 1, 4);
    check("two writes from one buffer do not wait for each other",
          resolves() == resolves_before);
    wait_call(fourth);
    wait_call(fifth);

    /* A read-only object cannot be stored into, so reading it need not wait
     * for the calls reading it. */
    void* sixth = write_at(fd, literal, 2, 5);
    hooks_before = hooks();
    char lit = literal[1];
    int waited = hooks() > hooks_before;
    printf("  (the read-only literal %s)\n",
           waited ? "was flagged: the read waited" : "had no flag: no wait");
    check("reading a read-only input sees its bytes", lit == 'y');
    check("the write from it completes", wait_call(sixth) == 2);

    close(fd);
    unlink(path);
    free(msg);
    if (failures)
        return 1;
    puts("T_HOST_ACCESS_AFTER_READ PASS");
    return 0;
}
