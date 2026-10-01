/* tests/io_uring/t_annotation_macros.c -- calls annotated with FILC_ASYNC
 * dispatch end to end.
 *
 * Covers the three ways a function gets its annotation: one FILC_ASYNC per
 * function, a #define that several functions share, and an annotated
 * declaration whose definition carries none. The write, the read and the
 * fsync name the same descriptor in FILC_W_DEP / FILC_R_DEP, so they run in
 * call order, and the program never waits between them.
 *
 * Usage: t_annotation_macros [dir]
 */
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "filc_async.h"

FILC_ASYNC(io_uring, FILC_OP(pwrite), FILC_BIN(buf), FILC_W_DEP(fd, file))
void* write_at(int fd, const void* buf, size_t len, unsigned long offset)
{
    return 0;
}

#define ASYNC_READ \
    FILC_ASYNC(io_uring, FILC_OP(pread), FILC_BOUT(buf), FILC_R_DEP(fd, file))

ASYNC_READ void* read_first(int fd, void* buf, size_t len, unsigned long offset)
{
    return 0;
}

ASYNC_READ void* read_second(int fd, void* buf, size_t len, unsigned long offset)
{
    return 0;
}

FILC_ASYNC(io_uring, FILC_OP(fsync), FILC_W_DEP(fd, file))
void* sync_file(int fd);

void* sync_file(int fd)
{
    return 0;
}

static long result_of(void* task)
{
    struct filc_async_result_s r = { .pending = task };
    filc_async_wait(&r);
    return r.state == 0 ? r.result : -1000;
}

int main(int argc, char** argv)
{
    static const char text[] = "annotated by macros";
    const size_t len = sizeof text - 1;
    char path[512];
    snprintf(path, sizeof path, "%s/t_annotation_macros.%ld.dat",
             argc > 1 ? argv[1] : "/tmp", (long)getpid());
    int fd = open(path, O_CREAT | O_TRUNC | O_RDWR, 0600);
    if (fd < 0) {
        perror(path);
        return 1;
    }
    unlink(path);

    char first[32] = { 0 };
    char second[32] = { 0 };
    void* wrote = write_at(fd, text, len, 0);
    void* synced = sync_file(fd);
    void* read_a = read_first(fd, first, len, 0);
    void* read_b = read_second(fd, second, len, 0);

    long w = result_of(wrote);
    long s = result_of(synced);
    long a = result_of(read_a);
    long b = result_of(read_b);

    int sizes_ok = w == (long)len && s == 0 && a == (long)len && b == (long)len;
    int contents_ok = strcmp(first, text) == 0 && strcmp(second, text) == 0;

    filc_async_stats stats;
    filc_async_get_stats(&stats);
    int counted = stats.tasks_submitted == 4 && stats.tasks_completed == 4 &&
                  stats.tasks_failed == 0;

    close(fd);
    int ok = sizes_ok && contents_ok && counted;
    printf("T_ANNOTATION_MACROS %s (write=%ld fsync=%ld reads=%ld,%ld submitted=%lu)\n",
           ok ? "PASS" : "FAIL", w, s, a, b, stats.tasks_submitted);
    return ok ? 0 : 1;
}
