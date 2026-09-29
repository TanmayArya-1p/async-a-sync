/* tests/t_pragma_repeat_read.c -- every annotated call site submits, even
 * when several are identical.
 *
 * The annotated body below is pure, so if the optimizer ran before the
 * FilAsync pass it could fold identical call sites together or inline them
 * away, and the surviving task's completion would be reported for all of
 * them while the buffer is never written again. Four call sites read two
 * files into one buffer, poisoned before each read, so a site that did not
 * submit is caught by the poison left behind. */
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

#pragma clang attribute push(__attribute__((annotate("filc_async", "runtime=io_uring", "op=pread", "bout=buf"))), apply_to=function)
void* async_pread(int fd, void* buf, size_t len, unsigned long offset)
{
    (void)fd;
    (void)buf;
    (void)offset;
    return (void*)(long)len;
}
#pragma clang attribute pop

static long finish(void* task, unsigned char* state)
{
    struct filc_async_result_s r = { 0 };
    r.pending = task;
    filc_async_wait(&r);
    *state = r.state;
    return r.result;
}

static size_t count_byte(const unsigned char* buf, unsigned char want)
{
    size_t n = 0;
    for (size_t i = 0; i < SZ; ++i)
        n += buf[i] == want;
    return n;
}

static int fill(const char* path, unsigned char pattern)
{
    int fd = open(path, O_CREAT | O_TRUNC | O_RDWR, 0600);
    unsigned char* src = malloc(SZ);
    int ok = fd >= 0 && src;
    if (ok) {
        memset(src, pattern, SZ);
        ok = write(fd, src, SZ) == (ssize_t)SZ;
    }
    free(src);
    if (fd >= 0)
        close(fd);
    return ok ? 0 : -1;
}

int main(int argc, char** argv)
{
    const char* dir = argc > 1 ? argv[1] : "/tmp";
    char pa[512];
    char pb[512];
    snprintf(pa, sizeof pa, "%s/t_pragma_repeat_read_a_%ld.dat", dir, (long)getpid());
    snprintf(pb, sizeof pb, "%s/t_pragma_repeat_read_b_%ld.dat", dir, (long)getpid());
    if (fill(pa, PAT_A) || fill(pb, PAT_B))
        return 2;

    int fd_a = open(pa, O_RDONLY);
    int fd_b = open(pb, O_RDONLY);
    unsigned char* buf = malloc(SZ);
    if (fd_a < 0 || fd_b < 0 || !buf)
        return 2;

    int failures = 0;
    /* Sites 1 and 3 are identical calls, as are 2 and 4; each must still
     * submit and read for itself. A macro, so each is its own call site. */
#define READ_AND_CHECK(idx, fd, want)                                         \
    do {                                                                      \
        unsigned char state = 0;                                              \
        memset(buf, POISON, SZ);                                              \
        long res = finish(async_pread((fd), buf, SZ, 0), &state);             \
        if (state != 0 || res != (long)SZ || count_byte(buf, (want)) != SZ) { \
            printf("!!! read %d: state=%u result=%ld poison left=%zu\n",      \
                   (idx), (unsigned)state, res, count_byte(buf, POISON));     \
            ++failures;                                                       \
        }                                                                     \
    } while (0)

    READ_AND_CHECK(1, fd_a, PAT_A);
    READ_AND_CHECK(2, fd_b, PAT_B);
    READ_AND_CHECK(3, fd_a, PAT_A);
    READ_AND_CHECK(4, fd_b, PAT_B);
#undef READ_AND_CHECK

    free(buf);
    close(fd_a);
    close(fd_b);
    unlink(pa);
    unlink(pb);
    printf("T_PRAGMA_REPEAT_READ %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
