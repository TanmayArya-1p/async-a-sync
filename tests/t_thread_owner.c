/* tests/t_thread_owner.c -- a request from a second thread is rejected.
 *
 * The runtime is single-threaded: one io_uring ring and one request table
 * with no locks. The thread that sets up the ring owns it. A request from any
 * other thread must stop the program with a clear message rather than race
 * the owner on the ring. tests/run.sh expects that message; exiting normally
 * is a failure.
 */
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "fasync.h"

static int g_fd;
static char g_buf[8];

static void* other_thread(void* arg)
{
    (void)arg;
    fasync_id id = fasync_pread(g_fd, g_buf, sizeof g_buf, 0);
    if (id)
        fasync_result(id);
    return NULL;
}

int main(int argc, char** argv)
{
    const char* dir = argc > 1 ? argv[1] : "/tmp";
    char path[512];
    if (snprintf(path, sizeof path, "%s/t_thread_owner_%ld.dat",
                 dir, (long)getpid()) >= (int)sizeof path)
        return 1;
    g_fd = open(path, O_CREAT | O_TRUNC | O_RDWR, 0600);
    if (g_fd < 0 || write(g_fd, "owner!!!", 8) != 8)
        return 1;
    unlink(path);

    /* the main thread makes the first request, so it owns the ring */
    char mine[8];
    fasync_id id = fasync_pread(g_fd, mine, sizeof mine, 0);
    if (!id || fasync_result(id) != 8)
        return 1;

    pthread_t t;
    if (pthread_create(&t, NULL, other_thread, NULL) != 0)
        return 1;
    pthread_join(t, NULL);

    puts("T_THREAD_OWNER FAIL: the second thread's request was accepted");
    return 1;
}
