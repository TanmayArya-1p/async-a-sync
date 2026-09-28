/* Deterministic request completion exposes which dependency may dispatch next. */
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fasync.h"
#include "filc_async.h"
#include "filc_async_alloc.h"

typedef union {
    void* ptr;
    uint64_t word;
} staged_arg;

static char token[32];
static char other_token[32];
static unsigned issued;
static int issue_fd[32];
static unsigned char done[32];
static int fail_next_fd = -1;

/* single-threaded, so the real runtime's owner check has nothing to do */
void fasync_check_thread(void) {}

void* zgetlower(void* ptr)
{
    uintptr_t p = (uintptr_t)ptr;
    if (p >= (uintptr_t)token && p < (uintptr_t)(token + sizeof token))
        return token;
    if (p >= (uintptr_t)other_token &&
        p < (uintptr_t)(other_token + sizeof other_token))
        return other_token;
    return ptr;
}

void* zgetupper(void* ptr)
{
    return (char*)zgetlower(ptr) + sizeof token;
}

long zsys_write(int fd, const void* buf, size_t len)
{
    return (long)fwrite(buf, 1, len, fd == 2 ? stderr : stdout);
}

void zsys_abort(void) { abort(); }

void* filc_async_alloc(size_t size, size_t align)
{
    (void)align;
    return calloc(1, size);
}

static fasync_id issue(int fd)
{
    assert(issued + 1 < sizeof issue_fd / sizeof issue_fd[0]);
    issue_fd[++issued] = fd;
    return issued;
}

fasync_id fasync_fsync(int fd)
{
    if (fd == fail_next_fd) {
        fail_next_fd = -1;
        errno = EBADF;
        return 0;
    }
    return issue(fd);
}
fasync_id fasync_pwrite(int fd, void* buf, size_t len, unsigned long offset)
{
    (void)buf;
    (void)len;
    (void)offset;
    return issue(fd);
}
fasync_id fasync_pread(int fd, void* buf, size_t len, unsigned long offset)
{
    (void)buf;
    (void)len;
    (void)offset;
    return issue(fd);
}
fasync_id fasync_openat(int fd, const char* path, int flags, int mode)
{
    (void)path;
    (void)flags;
    (void)mode;
    return issue(fd);
}
fasync_id fasync_close(int fd) { return issue(fd); }
int fasync_submit(void) { return 0; }
int fasync_ready(fasync_id id) { return done[id]; }
long fasync_result(fasync_id id) { done[id] = 1; return 0; }
void* fasync_resolve_pending(void* ptr, size_t size)
{
    (void)size;
    return ptr;
}
void fasync_get_stats(struct fasync_stats* out)
{
    memset(out, 0, sizeof *out);
    out->sqes_queued = issued;
}

static filc_async_meta* meta(const char* name, const char* const* opts,
                             unsigned nargs)
{
    filc_async_meta* m = filc_async_alloc(
        sizeof *m + nargs * sizeof m->args[0], 16);
    assert(m);
    m->name = name;
    m->opts = opts;
    m->nargs = nargs;
    m->noped_args = nargs == 1 ? 1 : 2;
    m->result = FILC_ASYNC_RESULT_PTR;
    m->args[0].kind = FILC_ASYNC_ARG_FD;
    if (nargs > 1)
        m->args[1].kind = FILC_ASYNC_ARG_BUFFER_IN;
    return m;
}

static void* send_fd(const filc_async_meta* m, int fd)
{
    staged_arg* a = filc_async_alloc(sizeof *a, 16);
    assert(a);
    a[0].word = (unsigned)fd;
    return filc_async_submit(m, NULL, NULL, a, 1);
}

static void* send_two(const filc_async_meta* m, int fd, void* ptr)
{
    staged_arg* a = filc_async_alloc(4 * sizeof *a, 16);
    assert(a);
    a[0].word = (unsigned)fd;
    a[1].ptr = ptr;
    return filc_async_submit(m, NULL, NULL, a, 4);
}

static void finish(void* task, unsigned id)
{
    done[id] = 1;
    struct filc_async_result_s result = { .pending = task };
    assert(filc_async_poll(&result));
    assert(result.state == 0 && result.result == 0);
    /* Delivering the completion retired the handle. */
    assert(!filc_async_poll(&result));
    assert(result.state == 0 && result.result == 0);
}

int main(void)
{
    static const char* const sync_opts[] = { "op=fsync", NULL };
    static const char* const write_opts[] = { "op=pwrite", NULL };
    filc_async_meta* read = meta("read", sync_opts, 1);
    filc_async_meta* write = meta("write", sync_opts, 1);
    read->args[0].dependency = FILC_ASYNC_DEP_READ;
    write->args[0].dependency = FILC_ASYNC_DEP_WRITE;
    const filc_async_meta* table[] = { read, write, NULL };
    filc_async_validate_table(table);

    void* r1 = send_fd(read, 7);
    void* r2 = send_fd(read, 7);
    assert(issued == 2 && issue_fd[1] == 7 && issue_fd[2] == 7);
    void* w1 = send_fd(write, 7);
    assert(issued == 3 && issue_fd[3] == 7 && done[1] && done[2]);
    void* w2 = send_fd(write, 7);
    assert(issued == 4 && issue_fd[4] == 7 && done[3]);
    void* r3 = send_fd(read, 7);
    assert(issued == 5 && issue_fd[5] == 7 && done[4]);
    void* independent = send_fd(write, 8);
    assert(issued == 6 && issue_fd[6] == 8);

    finish(r1, 1);
    finish(r2, 2);
    finish(w1, 3);
    finish(w2, 4);
    finish(r3, 5);
    finish(independent, 6);

    filc_async_meta* ptr_write = meta("ptr_write", write_opts, 4);
    filc_async_meta* ptr_read = meta("ptr_read", write_opts, 4);
    ptr_write->args[0].dependency = FILC_ASYNC_DEP_READ;
    ptr_write->args[1].dependency = FILC_ASYNC_DEP_WRITE |
                                    FILC_ASYNC_DEP_POINTER;
    ptr_read->args[1].dependency = FILC_ASYNC_DEP_READ |
                                   FILC_ASYNC_DEP_POINTER;
    const filc_async_meta* ptr_table[] = { ptr_write, ptr_read, NULL };
    filc_async_validate_table(ptr_table);

    void* p1 = send_two(ptr_write, 9, token + 1);
    void* p2 = send_two(ptr_read, 10, token + 8);
    void* p3 = send_two(ptr_read, 11, other_token);
    assert(issued == 9 && issue_fd[7] == 9 && issue_fd[8] == 10 &&
           issue_fd[9] == 11 && done[7]);
    finish(p1, 7);
    finish(p2, 8);
    finish(p3, 9);

    /* The same operation's second dependency is the scalar fd at arg 0. */
    void* p4 = send_two(ptr_write, 12, token + 1);
    void* scalar_conflict = send_fd(write, 12);
    assert(issued == 11 && issue_fd[10] == 12 && issue_fd[11] == 12 &&
           done[10]);
    finish(p4, 10);
    finish(scalar_conflict, 11);

    /* The second submit waits for the first task before returning its SQE. */
    void* wait_first = send_fd(write, 13);
    void* wait_second = send_fd(write, 13);
    assert(issued == 13 && done[12]);
    struct filc_async_result_s result = { .pending = wait_second };
    filc_async_wait(&result);
    assert(result.state == 0 && result.result == 0 && issued == 13);
    result.pending = wait_first;
    filc_async_wait(&result);
    assert(result.state == 0 && result.result == 0);

    void* before_failure = send_fd(write, 14);
    fail_next_fd = 14;
    void* failed = send_fd(write, 14);
    void* after_failure = send_fd(write, 14);
    assert(issued == 15 && done[14]);
    finish(before_failure, 14);
    assert(issue_fd[15] == 14);
    result.pending = failed;
    assert(filc_async_poll(&result));
    assert(result.state == 2 && result.result == -EBADF);
    finish(after_failure, 15);

    filc_async_stats stats;
    filc_async_get_stats(&stats);
    assert(stats.tasks_submitted == 16 && stats.tasks_completed == 16 &&
           stats.tasks_failed == 1 && stats.sqes_queued == 15);
    puts("CHECK_DEPENDENCIES PASS");
    return 0;
}
