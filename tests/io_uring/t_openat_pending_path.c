/* tests/io_uring/t_openat_pending_path.c -- an openat whose path is partly produced by
 * a pending read.
 *
 * The program writes the directory prefix of a path itself and reads the file
 * name into the rest of the buffer asynchronously. fasync_openat hands the
 * path to the kernel, so every byte up to the terminator has to be final
 * first, not just the first one: the pending read starts past byte 0.
 *
 * Reads are queued lazily, so without that the read has not even reached the
 * kernel when openat scans the path. The buffer is pre-filled with 'X' (no
 * terminator), which makes that failure deterministic rather than a race.
 */
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "fasync.h"

#define NAME "t_openat_pending_path.target"
#define CONTENT "target contents"

int main(int argc, char** argv)
{
    const char* dir = argc > 1 ? argv[1] : "/tmp";
    char target[512], names[512];
    if (snprintf(target, sizeof target, "%s/%s", dir, NAME) >= (int)sizeof target ||
        snprintf(names, sizeof names, "%s/t_openat_pending_path.names", dir) >=
            (int)sizeof names)
        return 1;

    /* the file to open, and a file holding its name with the terminator */
    int t = open(target, O_CREAT | O_TRUNC | O_WRONLY, 0600);
    int n = open(names, O_CREAT | O_TRUNC | O_RDWR, 0600);
    if (t < 0 || n < 0 ||
        write(t, CONTENT, sizeof CONTENT - 1) != (long)(sizeof CONTENT - 1) ||
        write(n, NAME, sizeof NAME) != (long)sizeof NAME) {
        perror("t_openat_pending_path: setup");
        return 1;
    }
    close(t);

    static char path[1024];
    memset(path, 'X', sizeof path);
    size_t k = strlen(dir);
    memcpy(path, dir, k);
    path[k++] = '/';
    fasync_id read_id = fasync_pread(n, path + k, sizeof NAME, 0);

    fasync_id open_id = fasync_openat(AT_FDCWD, path, O_RDONLY, 0);
    long fd = open_id ? fasync_result(open_id) : -1;
    long got = read_id ? fasync_result(read_id) : -1;

    char back[sizeof CONTENT] = { 0 };
    int ok = open_id && fd >= 0 && got == (long)sizeof NAME &&
             read((int)fd, back, sizeof back - 1) == (long)(sizeof CONTENT - 1) &&
             strcmp(back, CONTENT) == 0;
    if (!open_id)
        perror("t_openat_pending_path: fasync_openat");

    if (fd >= 0)
        close((int)fd);
    close(n);
    unlink(target);
    unlink(names);
    printf("T_OPENAT_PENDING_PATH %s (open=%lu fd=%ld read=%ld)\n",
           ok ? "PASS" : "FAIL", open_id, fd, got);
    return ok ? 0 : 1;
}
