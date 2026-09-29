// demos/demo_pragma_mixdeps.c -- two annotated calls carrying BOTH kinds of
// dependency key: a scalar (r_dep=0 on the fd) and a pointer (w_dep=1 on the
// buffer).
//
// This is the case that proves the two key types are independent mechanisms.
// The buffers are distinct, so the pointer keys never match and the pointer
// half contributes nothing. The only thing that can order the pair is the
// scalar fd, and only because the write claims a WRITE on it while the read
// claims a READ. A dependency needs at least one side to be a write, so
// changing the pwrite's w_dep=0 to r_dep=0 makes both calls readers and the
// pair stops being ordered.

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "filc_async.h"

#define SZ (16u << 10)

static int failures;
static volatile int body_calls;

#pragma clang attribute push(__attribute__((annotate("filc_async", "op=pwrite", "fd=0", "bin=1", "w_dep=0", "w_dep=1"))), apply_to=function)
__attribute__((noinline)) void* async_pwrite(int fd, const void* buf, size_t len, unsigned long offset);
#pragma clang attribute pop

#pragma clang attribute push(__attribute__((annotate("filc_async", "op=pread", "fd=0", "bout=1", "r_dep=0", "w_dep=1"))), apply_to=function)
__attribute__((noinline)) void* async_pread(int fd, void* buf, size_t len, unsigned long offset);
#pragma clang attribute pop

__attribute__((noinline)) void* async_pwrite(int fd, const void* buf, size_t len, unsigned long offset)
{
    body_calls++;
    return (void*)(long)len;
}

__attribute__((noinline)) void* async_pread(int fd, void* buf, size_t len, unsigned long offset)
{
    body_calls++;
    return (void*)(long)len;
}

static void check(int cond, const char* what)
{
    printf("  %s  %s\n", cond ? "ok  " : "FAIL", what);
    if (!cond)
        failures++;
}

static long finish(void* task, unsigned char* state)
{
    struct filc_async_result_s r = { .pending = task };
    if (!filc_async_poll(&r))
        filc_async_wait(&r);
    *state = r.state;
    return r.result;
}

int main(int argc, char** argv)
{
    const char* dir = argc > 1 ? argv[1] : "/tmp";
    char path[512];
    snprintf(path, sizeof path, "%s/demo_pragma_mixdeps_%ld.dat", dir, (long)getpid());

    int seed = open(path, O_CREAT | O_TRUNC | O_RDWR, 0600);
    if (seed < 0) {
        perror("open seed");
        return 2;
    }
    unsigned char* seed_bytes = malloc(SZ);
    memset(seed_bytes, '.', SZ);
    if (write(seed, seed_bytes, SZ) != (ssize_t)SZ) {
        perror("write seed");
        return 2;
    }
    close(seed);

    printf("pragma async: scalar and pointer keys together\n");

    int fd = open(path, O_RDWR);
    check(fd >= 0, "opened the seed file");
    if (fd < 0)
        return 2;

    unsigned char* src = malloc(SZ);
    unsigned char* dst = malloc(SZ);
    memset(src, 'B', SZ);
    memset(dst, 0x11, SZ);

    // Equal scalar fd, distinct buffers. The pointer keys differ, so the only
    // thing ordering this pair is the fd: w_dep=0 against r_dep=0.
    printf("\n  same fd, distinct buffers (ordered by the scalar key):\n");
    void* w = async_pwrite(fd, src, SZ, 0);
    void* r = async_pread(fd, dst, SZ, 0);
    check(w != NULL && r != NULL, "both submitted");

    unsigned char s1 = 0, s2 = 0;
    long rw = finish(w, &s1);
    long rr = finish(r, &s2);
    check(s1 == 0 && rw == (long)SZ, "write ok");
    check(s2 == 0 && rr == (long)SZ, "read ok");

    int saw = 0;
    for (size_t i = 0; i < SZ; i++)
        if (dst[i] == 'B') { saw = 1; break; }
    check(saw, "read observed the write (w_dep=0 vs r_dep=0 serialized the pair)");

    // Different fd: the scalar keys now differ, so nothing orders the pair and
    // no content claim is made -- only that both calls still succeed.
    printf("\n  different fd (no ordering promised):\n");
    int other = open(path, O_RDONLY);
    check(other >= 0, "opened a second fd on the same file");
    void* w2 = async_pwrite(fd, src, SZ, 0);
    void* r2 = async_pread(other, dst, SZ, 0);
    unsigned char s3 = 0, s4 = 0;
    long rw2 = finish(w2, &s3);
    long rr2 = finish(r2, &s4);
    check(s3 == 0 && rw2 == (long)SZ, "write ok");
    check(s4 == 0 && rr2 == (long)SZ, "read ok");

    check(body_calls == 0, "no annotated body executed");

    filc_async_stats st;
    filc_async_get_stats(&st);
    check(st.tasks_submitted == 4 && st.tasks_completed == 4 && st.tasks_failed == 0,
          "4 submitted, 4 completed, 0 failed");

    printf("\n%s\n", failures ? "DEMO FAILED" : "DEMO OK");
    free(src);
    free(dst);
    free(seed_bytes);
    close(fd);
    close(other);
    unlink(path);
    return failures ? 1 : 0;
}
