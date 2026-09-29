// Every annotated call site must submit, even when several call the same
// function with identical arguments into the same buffer. The pure stub body
// is what made the call sites foldable, which is the regression this covers;
// poisoning the buffer makes a phantom completion unsatisfiable. See the wiki.
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "filc_async.h"

#define SZ (64u << 10)
#define POISON 0x11
#define PAT_A 0x45
#define PAT_B 0x99

#pragma clang attribute push(__attribute__((annotate("filc_async", "op=pread", "fd=0", "bout=1"))), apply_to=function)
__attribute__((noinline)) void* async_pread(int fd, void* buf, size_t len, unsigned long offset)
{
    return (void*)(long)len;
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

static size_t count_byte(const unsigned char* buf, unsigned char want)
{
    size_t n = 0;
    for (size_t i = 0; i < SZ; ++i)
        n += (buf[i] == want);
    return n;
}

static int fill(const char* path, unsigned char pattern)
{
    int fd = open(path, O_CREAT | O_TRUNC | O_RDWR, 0600);
    if (fd < 0)
        return -1;
    unsigned char* src = malloc(SZ);
    if (!src) {
        close(fd);
        return -1;
    }
    memset(src, pattern, SZ);
    int ok = (write(fd, src, SZ) == (ssize_t)SZ);
    free(src);
    close(fd);
    return ok ? 0 : -1;
}

int main(void)
{
    const char* pa = "/tmp/t_pragma_repeat_read_a.dat";
    const char* pb = "/tmp/t_pragma_repeat_read_b.dat";
    if (fill(pa, PAT_A) || fill(pb, PAT_B))
        return 2;

    int fd_a = open(pa, O_RDONLY);
    int fd_b = open(pb, O_RDONLY);
    if (fd_a < 0 || fd_b < 0)
        return 2;
    unsigned char* buf = malloc(SZ);
    if (!buf)
        return 2;

    int failures = 0;
    // Sites 1/3 share (fd_a, buf, SZ, 0), 2/4 share (fd_b, buf, SZ, 0). Each
    // must still reach filc_async_submit and read for itself.
#define CHECK(idx, fd, want)                                                     \
    do {                                                                         \
        unsigned char state = 0;                                                 \
        memset(buf, POISON, SZ);                                                 \
        long res = resolve(async_pread((fd), buf, SZ, 0), &state);               \
        size_t matched = count_byte(buf, (want));                                \
        if (state != 0 || res != (long)SZ || matched != SZ) {                    \
            printf("!!! read %d: state=%u res=%ld want_bytes=%zu poison_left=%zu\n", \
                   (idx), (unsigned)state, res, matched,                         \
                   count_byte(buf, POISON));                                     \
            ++failures;                                                          \
        }                                                                        \
    } while (0)

    CHECK(1, fd_a, PAT_A);
    CHECK(2, fd_b, PAT_B);
    CHECK(3, fd_a, PAT_A);
    CHECK(4, fd_b, PAT_B);
#undef CHECK

    free(buf);
    close(fd_a);
    close(fd_b);
    unlink(pa);
    unlink(pb);

    if (failures) {
        printf("t_pragma_repeat_read: %d failure(s)\n", failures);
        return 1;
    }
    printf("t_pragma_repeat_read: all 4 call sites submitted and read\n");
    return 0;
}
