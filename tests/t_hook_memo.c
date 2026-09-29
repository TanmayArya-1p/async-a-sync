/* tests/t_hook_memo.c -- the access hook's memo must survive a loop that
 * alternates between two places in memory.
 *
 * While any request is in flight, every access goes through the compiler's
 * hook, and an access the memo does not cover scans the request table. The
 * loop below reads a heap buffer and a global in turn while two reads stay
 * queued, into buffers of the data's size allocated just before and just
 * after it (so from the same part of the heap), so a pending buffer lies
 * between the two places the loop reads wherever the globals are. If the memo
 * can only describe one range, each access evicts the other's and nearly
 * every access scans; it has to keep both.
 *
 * Needs the patched compiler and -DFASYNC_COMPILER_INSERTS_CHECKS. */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "fasync.h"

#ifndef FASYNC_COMPILER_INSERTS_CHECKS
#error "build with the patched compiler and -DFASYNC_COMPILER_INSERTS_CHECKS"
#endif

#define DATA 4096
#define ROUNDS 16

static volatile unsigned long scale = 3;

int main(int argc, char** argv)
{
    const char* dir = argc > 1 ? argv[1] : "/tmp";
    char path[512];
    if (snprintf(path, sizeof path, "%s/t_hook_memo_%ld.dat", dir,
                 (long)getpid()) >= (int)sizeof path)
        return 1;
    int fd = open(path, O_CREAT | O_TRUNC | O_RDWR, 0600);
    if (fd < 0 || pwrite(fd, "0123456789abcdef", 16, 0) != 16)
        return 1;

    unsigned char* before = malloc(DATA);
    unsigned char* data = malloc(DATA);
    unsigned char* after = malloc(DATA);
    if (!before || !data || !after)
        return 1;
    memset(data, 1, DATA);

    /* Queued, not submitted: both stay pending until fasync_result. */
    fasync_id a = fasync_pread(fd, before, 16, 0);
    fasync_id b = fasync_pread(fd, after, 16, 0);
    if (!a || !b)
        return 1;

    fasync_reset_stats();
    unsigned long sum = 0;
    for (int r = 0; r < ROUNDS; r++)
        for (size_t i = 0; i < DATA; i++)
            sum += data[i] * scale;
    struct fasync_stats s;
    fasync_get_stats(&s);

    long ra = fasync_result(a);
    long rb = fasync_result(b);
    close(fd);
    unlink(path);

    unsigned long misses = s.resolve_calls - s.memo_hits;
    int ok = sum == 3UL * DATA * ROUNDS && ra == 16 && rb == 16 &&
             s.resolve_calls >= (unsigned long)DATA * ROUNDS && misses < 64;
    printf("T_HOOK_MEMO %s (checked accesses=%lu memo hits=%lu misses=%lu)\n",
           ok ? "PASS" : "FAIL", s.resolve_calls, s.memo_hits, misses);
    return ok ? 0 : 1;
}
