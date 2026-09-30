/* tests/framework/t_two_runtimes.c -- one program, two runtimes.
 *
 * Linked with the io_uring runtime and tests/support/mock_runtime.c. write_at and
 * read_at name runtime=io_uring; check_file and sum_bytes name runtime=mock,
 * whose submit runs the body at once. The runtimes know nothing of each
 * other; what connects their calls is the framework:
 *
 *   - check_file read-locks the fd that write_at write-locked, so its stub
 *     waits for the write, polling it through the io_uring runtime, before
 *     the mock runtime runs it;
 *   - sum_bytes reads a buffer an io_uring read still owns, so the access in
 *     its body waits for that read, again through the io_uring runtime. */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "filc_async.h"

#pragma clang attribute push(__attribute__((annotate("filc_async", "runtime=io_uring", "op=pwrite", "bin=buf", "w_dep=fd:file"))), apply_to=function)
void* write_at(int fd, const void* buf, size_t len, unsigned long offset);
#pragma clang attribute pop

#pragma clang attribute push(__attribute__((annotate("filc_async", "runtime=io_uring", "op=pread", "bout=buf"))), apply_to=function)
void* read_at(int fd, void* buf, size_t len, unsigned long offset);
#pragma clang attribute pop

#pragma clang attribute push(__attribute__((annotate("filc_async", "runtime=mock", "op=check", "r_dep=fd:file", "bout=out"))), apply_to=function)
void* check_file(int fd, char* out, size_t len)
{
    return (void*)(intptr_t)pread(fd, out, len, 0);
}
#pragma clang attribute pop

#pragma clang attribute push(__attribute__((annotate("filc_async", "runtime=mock", "op=sum", "bin=in"))), apply_to=function)
void* sum_bytes(const char* in, size_t len)
{
    uintptr_t sum = 0;
    for (size_t i = 0; i < len; ++i)
        sum += (unsigned char)in[i];
    return (void*)sum;
}
#pragma clang attribute pop

void* write_at(int fd, const void* buf, size_t len, unsigned long offset)
{
    return (void*)(intptr_t)(fd + (buf != NULL) + len + offset);
}

void* read_at(int fd, void* buf, size_t len, unsigned long offset)
{
    return (void*)(intptr_t)(fd + (buf != NULL) + len + offset);
}

static long finish(void* task)
{
    struct filc_async_result_s r = { .pending = task };
    filc_async_wait(&r);
    return r.state == 0 ? r.result : -1;
}

int main(int argc, char** argv)
{
    const char* dir = argc > 1 ? argv[1] : "/tmp";
    char path[512];
    if (snprintf(path, sizeof path, "%s/t_two_runtimes_%ld.dat", dir,
                 (long)getpid()) >= (int)sizeof path)
        return 1;
    int fd = open(path, O_CREAT | O_TRUNC | O_RDWR, 0600);
    if (fd < 0)
        return 2;
    unlink(path);

    static char got[8];
    static char buf[8];
    void* wrote = write_at(fd, "hello", 5, 0);  /* io_uring, queued */
    void* checked = check_file(fd, got, 5);     /* mock, after the write */
    void* read = read_at(fd, buf, 5, 0);        /* io_uring, queued */
    void* summed = sum_bytes(buf, 5);           /* mock, waits for the read */

    long w = finish(wrote), c = finish(checked), r = finish(read),
         s = finish(summed);
    filc_async_stats stats;
    filc_async_get_stats(&stats);
    close(fd);

    long expect = 'h' + 'e' + 'l' + 'l' + 'o';
    int ok = w == 5 && c == 5 && r == 5 && s == expect &&
             memcmp(got, "hello", 5) == 0 && memcmp(buf, "hello", 5) == 0 &&
             stats.tasks_submitted == 4 && stats.tasks_completed == 4 &&
             stats.lock_waits >= 1 && stats.hook_resolves >= 1;
    printf("T_TWO_RUNTIMES %s (write=%ld check=%ld read=%ld sum=%ld "
           "lock waits=%lu hook resolves=%lu)\n",
           ok ? "PASS" : "FAIL", w, c, r, s, stats.lock_waits,
           stats.hook_resolves);
    return ok ? 0 : 1;
}
