/* Options name parameters, and a dependency's key is the argument's value
 * together with the parameter's name and the namespace.
 *
 * pn_write is annotated on a prototype with unnamed parameters, as in a
 * header; its names come from the definition. Calls on one fd conflict only
 * when their dependencies agree in both parameter name and namespace:
 *   pn_write  w_dep=fd:file      pn_read    r_dep=fd:file      conflict
 *   pn_meta   w_dep=fd:meta      (another namespace)           independent
 *   pn_other  w_dep=handle:file  (another parameter name)      independent */
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "filc_async.h"

#pragma clang attribute push(__attribute__((annotate("filc_async", "runtime=io_uring", "op=pwrite", "bin=buf", "w_dep=fd:file"))), apply_to=function)
void* pn_write(int, const void*, size_t, unsigned long);
#pragma clang attribute pop

#pragma clang attribute push(__attribute__((annotate("filc_async", "runtime=io_uring", "op=pread", "bout=buf", "r_dep=fd:file"))), apply_to=function)
void* pn_read(int fd, void* buf, size_t len, unsigned long offset) { return 0; }
#pragma clang attribute pop

#pragma clang attribute push(__attribute__((annotate("filc_async", "runtime=io_uring", "op=fsync", "w_dep=fd:meta"))), apply_to=function)
void* pn_meta(int fd) { return 0; }
#pragma clang attribute pop

#pragma clang attribute push(__attribute__((annotate("filc_async", "runtime=io_uring", "op=fsync", "w_dep=handle:file"))), apply_to=function)
void* pn_other(int handle) { return 0; }
#pragma clang attribute pop

void* pn_write(int fd, const void* buf, size_t len, unsigned long offset) { return 0; }

static unsigned long lock_waits(void)
{
    filc_async_stats stats;
    filc_async_get_stats(&stats);
    return stats.lock_waits;
}

int main(int argc, char** argv)
{
    const char* dir = argc > 1 ? argv[1] : "/tmp";
    char path[512];
    snprintf(path, sizeof path, "%s/t_param_names_%ld.dat", dir, (long)getpid());
    int fd = open(path, O_CREAT | O_TRUNC | O_RDWR, 0600);
    if (fd < 0)
        return 2;
    unlink(path);

    /* The read waits for the write: both lock fd:file. */
    char buf[8] = {0};
    pn_write(fd, "hello", 5, 0);
    pn_read(fd, buf, 5, 0);
    int read_ok = buf[0] == 'h' && buf[4] == 'o';
    unsigned long after_read = lock_waits();

    /* With a write holding fd:file, neither fd:meta nor handle:file waits. */
    pn_write(fd, "again", 5, 0);
    pn_meta(fd);
    pn_other(fd);
    unsigned long after_others = lock_waits();

    int ok = read_ok && after_read == 1 && after_others == 1;
    printf("T_PARAM_NAMES %s (read %s, lock waits %lu then %lu)\n",
           ok ? "PASS" : "FAIL", read_ok ? "ok" : "wrong", after_read,
           after_others);
    close(fd);
    return ok ? 0 : 1;
}
