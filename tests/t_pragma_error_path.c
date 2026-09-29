// Async ops on a bad fd must fail fast with -EBADF, never hang. The regression
// was submitting to the ring's own descriptor, which the kernel accepts and
// never completes. The alarm is deliberate: a regression here is a hang, and a
// hanging test would wedge the suite rather than fail it. See the wiki.
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "filc_async.h"

#define SZ (64u << 10)

static void on_alarm(int sig)
{
    (void)sig;
    static const char msg[] = "!!! t_pragma_error_path: TIMED OUT (an op on a bad fd hung)\n";
    ssize_t ignored = write(2, msg, sizeof msg - 1);
    (void)ignored;
    _exit(9);
}

#pragma clang attribute push(__attribute__((annotate("filc_async", "op=pread", "fd=0", "bout=1"))), apply_to=function)
__attribute__((noinline)) void* async_pread(int fd, void* buf, size_t len, unsigned long offset)
{
    return (void*)(long)len;
}
#pragma clang attribute pop

#pragma clang attribute push(__attribute__((annotate("filc_async", "op=fsync", "fd=0"))), apply_to=function)
__attribute__((noinline)) void* async_fsync(int fd)
{
    return (void*)(long)fd;
}
#pragma clang attribute pop

static long resolve(void* task, unsigned char* state)
{
    struct filc_async_result_s r = { 0 };
    r.pending = task;
    if (!filc_async_poll(&r))
        filc_async_wait(&r);
    *state = r.state;
    return r.result;
}

int main(void)
{
    const char* path = "/tmp/t_pragma_error_path.dat";
    signal(SIGALRM, on_alarm);
    alarm(30);

    int seed = open(path, O_CREAT | O_TRUNC | O_RDWR, 0600);
    if (seed < 0)
        return 2;
    unsigned char* src = malloc(SZ);
    memset(src, 'E', SZ);
    if (write(seed, src, SZ) != (ssize_t)SZ)
        return 3;
    // Close the lowest descriptor before the ring exists, so io_uring_setup is
    // guaranteed to hand back this exact number.
    close(seed);

    int failures = 0;
    unsigned char state = 0;
    unsigned char* buf = malloc(SZ);

    // 1. the collision case: this fd is closed and the ring will take it.
    memset(buf, 0, SZ);
    long res = resolve(async_pread(seed, buf, SZ, 0), &state);
    if (state != 2 || res != -EBADF) {
        printf("!!! closed fd %d: state=%u result=%ld (want state=2 result=%d)\n",
               seed, (unsigned)state, res, -EBADF);
        ++failures;
    }
    if (filc_async_is_pending(buf)) {
        printf("!!! buffer still marked pending after a failed op\n");
        ++failures;
    }

    // 2. an outright invalid descriptor.
    memset(buf, 0, SZ);
    res = resolve(async_pread(-1, buf, SZ, 0), &state);
    if (state != 2 || res != -EBADF) {
        printf("!!! fd=-1: state=%u result=%ld (want state=2 result=%d)\n",
               (unsigned)state, res, -EBADF);
        ++failures;
    }

    // 3. fsync on a bad fd takes the same path.
    res = resolve(async_fsync(-1), &state);
    if (state != 2 || res != -EBADF) {
        printf("!!! fsync fd=-1: state=%u result=%ld (want state=2 result=%d)\n",
               (unsigned)state, res, -EBADF);
        ++failures;
    }

    // 4. the runtime must still work after those failures.
    int good = open(path, O_RDONLY);
    if (good < 0)
        return 2;
    memset(buf, 0, SZ);
    res = resolve(async_pread(good, buf, SZ, 0), &state);
    if (state != 0 || res != (long)SZ || buf[0] != 'E') {
        printf("!!! valid read after failures: state=%u result=%ld buf[0]=%c\n",
               (unsigned)state, res, buf[0]);
        ++failures;
    }

    // 5. a read past EOF is a short read, not a failure.
    memset(buf, 0, SZ);
    res = resolve(async_pread(good, buf, SZ, SZ * 4), &state);
    if (state != 0 || res != 0) {
        printf("!!! read past EOF: state=%u result=%ld (want state=0 result=0)\n",
               (unsigned)state, res);
        ++failures;
    }

    close(good);
    unlink(path);
    free(buf);
    free(src);
    alarm(0);

    if (failures) {
        printf("t_pragma_error_path: %d failure(s)\n", failures);
        return 1;
    }
    printf("t_pragma_error_path: bad fds report -EBADF, valid ops still work\n");
    return 0;
}
