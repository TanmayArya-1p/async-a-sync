/* tests/t_pragma_error_path.c -- annotated calls on a bad fd fail with
 * -EBADF instead of hanging.
 *
 * The ring is set up by the first request and takes the lowest free
 * descriptor. Closing a file just before that hands the ring the file's old
 * number, and an SQE on the ring's own fd is accepted by the kernel and never
 * completed. A regression is a hang, so the test bounds itself with alarm()
 * rather than wedging the suite.
 *
 *   1. a read on the closed fd the ring now owns;
 *   2. a read and an fsync on fd -1;
 *   3. a valid read afterwards, and a read past EOF (a short read, not an
 *      error). */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "filc_async.h"

#define SZ (64u << 10)

#pragma clang attribute push(__attribute__((annotate("filc_async", "runtime=io_uring", "op=pread", "bout=buf"))), apply_to=function)
void* async_pread(int fd, void* buf, size_t len, unsigned long offset)
{
    (void)fd;
    (void)buf;
    (void)len;
    (void)offset;
    return 0;
}
#pragma clang attribute pop

#pragma clang attribute push(__attribute__((annotate("filc_async", "runtime=io_uring", "op=fsync"))), apply_to=function)
void* async_fsync(int fd)
{
    (void)fd;
    return 0;
}
#pragma clang attribute pop

static void on_alarm(int sig)
{
    (void)sig;
    static const char msg[] =
        "T_PRAGMA_ERROR_PATH FAIL (an annotated call on a bad fd hung)\n";
    ssize_t ignored = write(2, msg, sizeof msg - 1);
    (void)ignored;
    _exit(9);
}

static long finish(void* task, unsigned char* state)
{
    struct filc_async_result_s r = { 0 };
    r.pending = task;
    filc_async_wait(&r);
    *state = r.state;
    return r.result;
}

static int failures;

static void expect(const char* what, int ok)
{
    if (!ok) {
        printf("!!! %s\n", what);
        failures++;
    }
}

int main(int argc, char** argv)
{
    const char* dir = argc > 1 ? argv[1] : "/tmp";
    char path[512];
    if (snprintf(path, sizeof path, "%s/t_pragma_error_path_%ld.dat",
                 dir, (long)getpid()) >= (int)sizeof path)
        return 1;
    signal(SIGALRM, on_alarm);
    alarm(30);

    int seed = open(path, O_CREAT | O_TRUNC | O_RDWR, 0600);
    unsigned char* buf = malloc(SZ);
    if (seed < 0 || !buf)
        return 2;
    memset(buf, 'E', SZ);
    if (write(seed, buf, SZ) != (ssize_t)SZ)
        return 3;
    /* Free the lowest descriptor before the ring exists, so the ring takes
     * exactly this number. */
    close(seed);

    unsigned char state = 0;
    memset(buf, 0, SZ);
    long res = finish(async_pread(seed, buf, SZ, 0), &state);
    expect("a read on the closed fd the ring took failed with -EBADF",
           state == 2 && res == -EBADF);
    expect("its buffer is no longer pending", !filc_async_is_pending(buf));

    res = finish(async_pread(-1, buf, SZ, 0), &state);
    expect("a read on fd -1 failed with -EBADF", state == 2 && res == -EBADF);
    res = finish(async_fsync(-1), &state);
    expect("an fsync on fd -1 failed with -EBADF", state == 2 && res == -EBADF);

    int good = open(path, O_RDONLY);
    if (good < 0)
        return 2;
    memset(buf, 0, SZ);
    res = finish(async_pread(good, buf, SZ, 0), &state);
    expect("a valid read still works after the failures",
           state == 0 && res == (long)SZ && buf[0] == 'E');
    res = finish(async_pread(good, buf, SZ, SZ * 4), &state);
    expect("a read past EOF returns 0", state == 0 && res == 0);

    close(good);
    unlink(path);
    free(buf);
    alarm(0);
    printf("T_PRAGMA_ERROR_PATH %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
