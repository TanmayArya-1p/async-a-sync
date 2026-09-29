/* tests/t_pragma_same_tu_lazy.c -- an annotated definition in the calling
 * translation unit, used lazily.
 *
 * The body of lazy_pread is a side-effect-free stub, the result of the call is
 * dropped, and the buffer is read straight after the call. Nothing about the
 * stub may leak into the caller: if the optimizer runs before FilAsyncPass, it
 * either deletes the call (its result is unused and the stub does nothing) or
 * concludes that the call does not write `buf` and forwards the zeros from the
 * memset past it. Either way the read below would see zeros instead of the
 * file's contents.
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "filc_async.h"

#pragma clang attribute push(__attribute__((annotate("filc_async", "runtime=io_uring", "op=pread", "bout=1"))), apply_to=function)
void* lazy_pread(int fd, void* buf, size_t len, unsigned long offset)
{
    (void)fd;
    (void)buf;
    (void)len;
    (void)offset;
    return 0;
}
#pragma clang attribute pop

int main(int argc, char** argv)
{
    const char* dir = argc > 1 ? argv[1] : "/tmp";
    char path[512];
    if (snprintf(path, sizeof path, "%s/t_pragma_same_tu_lazy_%ld.dat",
                 dir, (long)getpid()) >= (int)sizeof path)
        return 1;
    int fd = open(path, O_CREAT | O_TRUNC | O_RDWR, 0600);
    if (fd < 0 || write(fd, "samefile", 8) != 8) {
        perror("t_pragma_same_tu_lazy: setup");
        return 1;
    }

    char* buf = malloc(8);
    if (!buf)
        return 1;
    memset(buf, 0, 8);
    lazy_pread(fd, buf, 8, 0);
    /* Compare in this translation unit, where the compiler's access hook
     * resolves the pending read; libc's memcmp is not instrumented. */
    int ok = 1;
    for (int i = 0; i < 8; i++)
        ok &= buf[i] == "samefile"[i];

    filc_async_stats stats;
    filc_async_get_stats(&stats);
    ok = ok && stats.tasks_submitted == 1;

    free(buf);
    close(fd);
    unlink(path);
    printf("T_PRAGMA_SAME_TU_LAZY %s (submitted=%lu)\n", ok ? "PASS" : "FAIL",
           stats.tasks_submitted);
    return ok ? 0 : 1;
}
