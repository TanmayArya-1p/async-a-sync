// demos/demo_pragma_ptrdeps.c -- two annotated calls whose only dependency is
// a POINTER key (w_dep=1 / r_dep=1).
//
// Two ops conflict when their pointer keys are equal, so a write and a read
// naming the same buffer must order against each other. This demo shows both
// sides of that rule: distinct buffers run freely, the same buffer serializes.

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "filc_async.h"

#define SZ (16u << 10)

static int failures;
static volatile int body_calls;

#pragma clang attribute push(__attribute__((annotate("filc_async", "op=pwrite", "fd=0", "bin=1", "w_dep=1"))), apply_to=function)
__attribute__((noinline)) void* async_pwrite(int fd, const void* buf, size_t len, unsigned long offset);
#pragma clang attribute pop

#pragma clang attribute push(__attribute__((annotate("filc_async", "op=pread", "fd=0", "bout=1", "r_dep=1"))), apply_to=function)
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
    snprintf(path, sizeof path, "%s/demo_pragma_ptrdeps_%ld.dat", dir, (long)getpid());

    int seed = open(path, O_CREAT | O_TRUNC | O_RDWR, 0600);
    if (seed < 0) {
        perror("open seed");
        return 2;
    }
    unsigned char* seed_bytes = malloc(SZ);
    memset(seed_bytes, 'A', SZ);
    if (write(seed, seed_bytes, SZ) != (ssize_t)SZ) {
        perror("write seed");
        return 2;
    }
    close(seed);

    printf("pragma async: pointer-key dependencies\n");

    int fd = open(path, O_RDWR);
    check(fd >= 0, "opened the seed file");
    if (fd < 0)
        return 2;

    // --- distinct buffers: the pointer keys differ, so nothing serializes ---
    // No ordering is promised here, so there is nothing to assert about what
    // the read sees -- only that both calls complete.
    unsigned char* b1 = malloc(SZ);
    unsigned char* b2 = malloc(SZ);
    memset(b1, 'A', SZ);
    memset(b2, 0x11, SZ);

    printf("\n  distinct buffers (independent, no ordering):\n");
    void* w = async_pwrite(fd, b1, SZ, 0);
    void* r = async_pread(fd, b2, SZ, 0);
    check(w != NULL && r != NULL, "both submitted");

    unsigned char s1 = 0, s2 = 0;
    long rw = finish(w, &s1);
    long rr = finish(r, &s2);
    check(s1 == 0 && rw == (long)SZ, "write ok");
    check(s2 == 0 && rr == (long)SZ, "read ok");

    // --- same buffer: the keys match, so the pair must order ---
    // Two SZ ranges in one allocation, since one call writes at offset SZ.
    printf("\n  same buffer (must serialize):\n");
    unsigned char* big = malloc(2 * SZ);
    memset(big, 0x22, 2 * SZ);
    void* w2 = async_pwrite(fd, big, SZ, SZ); // write to the back half
    void* r2 = async_pread(fd, big, SZ, 0);  // read the front half
    unsigned char s3 = 0, s4 = 0;
    long rw2 = finish(w2, &s3);
    long rr2 = finish(r2, &s4);
    check(s3 == 0 && rw2 == (long)SZ, "same-buffer write ok");
    check(s4 == 0 && rr2 == (long)SZ, "same-buffer read ok");

    // Front half still holds the first write's 'A'; the second write's 0x22
    // belongs at offset SZ. That is what ordered execution produces.
    int front_ok = 1, back_ok = 1;
    for (size_t i = 0; i < SZ; i++) {
        if (big[i] != 'A') front_ok = 0;
        if (big[SZ + i] != 0x22) back_ok = 0;
    }
    check(front_ok, "front half kept the first write (no reordering)");
    check(back_ok, "second write landed at offset SZ");

    check(body_calls == 0, "no annotated body executed");

    filc_async_stats st;
    filc_async_get_stats(&st);
    check(st.tasks_submitted == 4 && st.tasks_completed == 4 && st.tasks_failed == 0,
          "4 submitted, 4 completed, 0 failed");

    printf("\n%s\n", failures ? "DEMO FAILED" : "DEMO OK");
    free(b1);
    free(b2);
    free(big);
    free(seed_bytes);
    close(fd);
    unlink(path);
    return failures ? 1 : 0;
}
