#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

#include "filc_async.h"

#pragma clang attribute push(__attribute__((annotate("filc_async", "runtime=io_uring", "op=pread", "bout=data"))), apply_to=function)
void* joined_read(int fd, void* data, size_t len, unsigned long offset, prov_tag done)
{
    (void)fd;
    (void)data;
    (void)len;
    (void)offset;
    (void)done;
    return NULL;
}
#pragma clang attribute pop

#pragma clang attribute push(__attribute__((annotate("filc_async", "runtime=io_uring", "op=pwrite", "bin=data"))), apply_to=function)
void* joined_write(int fd, const void* data, size_t len, unsigned long offset, prov_tag done)
{
    (void)fd;
    (void)data;
    (void)len;
    (void)offset;
    (void)done;
    return NULL;
}
#pragma clang attribute pop

static long wait_result(void* task)
{
    struct filc_async_result_s result = { .pending = task };
    filc_async_wait(&result);
    return result.result;
}

int main(int argc, char** argv)
{
    static const char initial[] = "abcdefghijklmnopqrstuvwxyz1234";
    enum { READS = sizeof(initial) - 1 };
    const char* directory = argc > 1 ? argv[1] : "/tmp";
    char path[512];
    int len = snprintf(path, sizeof path, "%s/wait-all-%ld.dat",
                       directory, (long)getpid());
    if (len < 0 || (size_t)len >= sizeof path) {
        fputs("output path is too long\n", stderr);
        return 1;
    }

    int fd = open(path, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd < 0) {
        perror("open");
        return 1;
    }
    unlink(path);
    if (write(fd, initial, READS) != READS) {
        perror("write");
        close(fd);
        return 1;
    }

    void* reads[READS];
    prov_tag tags[READS];
    char* data[READS];
    for (unsigned i = 0; i < READS; ++i) {
        tags[i] = prov_alloc();
        data[i] = prov_alloc();
        if (!tags[i] || !data[i]) {
            fputs("allocation failed\n", stderr);
            close(fd);
            return 1;
        }
        reads[i] = joined_read(fd, data[i], 1, i, tags[i]);
    }
    puts("30 reads queued");

    void* group = filc_async_wait_all(tags, READS);
    printf("group pending: %s\n", filc_async_is_pending(group) ? "yes" : "no");
    void* writer = joined_write(fd, "!", 1, 0, group);
    puts("write handed off after the reads");

    for (unsigned i = 0; i < READS; ++i)
        if (wait_result(reads[i]) != 1) {
            fputs("read failed\n", stderr);
            close(fd);
            return 1;
        }
    if (wait_result(writer) != 1) {
        fputs("write failed\n", stderr);
        close(fd);
        return 1;
    }

    fputs("read bytes: ", stdout);
    for (unsigned i = 0; i < READS; ++i)
        putchar(*data[i]);
    putchar('\n');

    char first;
    if (pread(fd, &first, 1, 0) != 1) {
        perror("pread");
        close(fd);
        return 1;
    }
    printf("first byte after write: %c\n", first);
    close(fd);
    return 0;
}
