/* tests/t_thread_compute.c -- a thread that does no I/O stays out of the
 * runtime.
 *
 * The runtime is single-threaded, but a program may still run other threads
 * that only compute. Code built by the patched compiler calls the access hook
 * on every checked access. While the owner has a request in flight, that hook
 * leaves its lock-free fast path; on any thread but the owner it must return
 * straight away instead of reading and writing the runtime's shared state
 * (lookup memo, counters, rings), which it would race on.
 *
 * The owner queues a read and does not touch it (it stays in flight), a
 * second thread makes ACCESSES checked accesses to its own memory, and the
 * hook's slow-path counter must barely move. Then the owner reads its buffer,
 * which the hook resolves as usual.
 */
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "fasync.h"

#define ACCESSES 100000
#define WORDS 256

static void* compute(void* arg)
{
    unsigned long* words = arg;
    unsigned long sum = 0;
    for (int i = 0; i < ACCESSES; i++)
        sum += words[i % WORDS] + (unsigned long)i;
    words[0] = sum;
    return NULL;
}

int main(int argc, char** argv)
{
    const char* dir = argc > 1 ? argv[1] : "/tmp";
    char path[512];
    if (snprintf(path, sizeof path, "%s/t_thread_compute_%ld.dat",
                 dir, (long)getpid()) >= (int)sizeof path)
        return 1;
    int fd = open(path, O_CREAT | O_TRUNC | O_RDWR, 0600);
    if (fd < 0 || write(fd, "computed", 8) != 8)
        return 1;
    unlink(path);

    char* buf = malloc(8);
    unsigned long* words = calloc(WORDS, sizeof *words);
    if (!buf || !words)
        return 1;
    memset(buf, 0, 8);

    /* queued, not submitted: in flight until the owner touches buf */
    if (!fasync_pread(fd, buf, 8, 0))
        return 1;

    struct fasync_stats before, after;
    fasync_get_stats(&before);
    pthread_t t;
    if (pthread_create(&t, NULL, compute, words) != 0)
        return 1;
    pthread_join(t, NULL);
    fasync_get_stats(&after);
    /* The owner's own few accesses between the two snapshots count too;
     * the compute thread's would add about ACCESSES. */
    unsigned long resolves = after.resolve_calls - before.resolve_calls;

    int read_ok = 1;
    for (int i = 0; i < 8; i++)
        read_ok &= buf[i] == "computed"[i];

    int ok = resolves < 16 && read_ok;
    printf("T_THREAD_COMPUTE %s (slow-path resolves while the compute thread "
           "ran=%lu, read=%d)\n", ok ? "PASS" : "FAIL", resolves, read_ok);
    free(words);
    free(buf);
    close(fd);
    return ok ? 0 : 1;
}
