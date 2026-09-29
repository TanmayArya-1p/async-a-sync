/* The generic framework against a mock runtime: dependency locks decide
 * which call reaches the runtime next, and completion is deterministic. */
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "filc_async.h"
#include "filc_async_alloc.h"
#include "filc_async_runtime.h"

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

/* ---- The mock runtime: numbers each call it is handed, and completes call
 * n once done[n] is set or someone blocks on it. ---- */

void filc_async_submit(void* task, const filc_async_meta* meta,
                       filc_async_run_fn run, void* staged_args, size_t nargs)
{
    (void)meta;
    (void)run;
    (void)nargs;
    int fd = (int)((staged_arg*)staged_args)[0].word;
    if (fd == fail_next_fd) {
        fail_next_fd = -1;
        filc_async_complete(task, -EBADF);
        return;
    }
    assert(issued + 1 < sizeof issue_fd / sizeof issue_fd[0]);
    issue_fd[++issued] = fd;
    *filc_async_task_runtime_data(task) = (void*)(uintptr_t)issued;
}

bool filc_async_runtime_poll(void* task, enum filc_async_poll_mode mode)
{
    unsigned id = (unsigned)(uintptr_t)*filc_async_task_runtime_data(task);
    if (!done[id]) {
        if (mode != FILC_ASYNC_POLL_BLOCK)
            return false;
        done[id] = 1;
    }
    filc_async_complete(task, 0);
    return true;
}

bool filc_async_runtime_validate(const filc_async_meta* meta)
{
    (void)meta;
    return true;
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

/* What a pass-emitted stub does: start the task, lock each dependency
 * argument, hand the call to the runtime. */
static void* stub(const filc_async_meta* m, staged_arg* a)
{
    void* task = filc_async_begin(m, a);
    for (unsigned i = 0; i < m->nargs; ++i) {
        unsigned dep = m->args[i].dependency;
        unsigned mode = dep & (FILC_ASYNC_DEP_READ | FILC_ASYNC_DEP_WRITE);
        if (!mode)
            continue;
        unsigned space = dep & ~(FILC_ASYNC_DEP_READ | FILC_ASYNC_DEP_WRITE);
        if (dep & FILC_ASYNC_DEP_POINTER)
            filc_async_lock_ptr(task, a[i].ptr, space, mode);
        else
            filc_async_lock_word(task, a[i].word, space, mode);
    }
    filc_async_submit(task, m, NULL, a, m->nargs);
    return task;
}

static void* send_fd(const filc_async_meta* m, int fd)
{
    staged_arg* a = filc_async_alloc(sizeof *a, 16);
    assert(a);
    a[0].word = (unsigned)fd;
    return stub(m, a);
}

static void* send_two(const filc_async_meta* m, int fd, void* ptr)
{
    staged_arg* a = filc_async_alloc(4 * sizeof *a, 16);
    assert(a);
    a[0].word = (unsigned)fd;
    a[1].ptr = ptr;
    return stub(m, a);
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

    /* The second call waits in its stub for the first to release the lock. */
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

    /* A :<name> suffix puts a key in a namespace, hashed into bits 8..31.
     * Equal values order each other only within one namespace. */
    filc_async_meta* ns_write = meta("ns_write", sync_opts, 1);
    filc_async_meta* ns_read_left = meta("ns_read_left", sync_opts, 1);
    filc_async_meta* ns_read_right = meta("ns_read_right", sync_opts, 1);
    ns_write->args[0].dependency =
        FILC_ASYNC_DEP_WRITE | (0x111111u << FILC_ASYNC_DEP_NAMESPACE_SHIFT);
    ns_read_left->args[0].dependency =
        FILC_ASYNC_DEP_READ | (0x111111u << FILC_ASYNC_DEP_NAMESPACE_SHIFT);
    ns_read_right->args[0].dependency =
        FILC_ASYNC_DEP_READ | (0x222222u << FILC_ASYNC_DEP_NAMESPACE_SHIFT);
    const filc_async_meta* ns_table[] = {
        ns_write, ns_read_left, ns_read_right, NULL
    };
    filc_async_validate_table(ns_table);

    void* nw1 = send_fd(ns_write, 16);
    assert(issued == 16 && issue_fd[16] == 16);
    /* Same namespace, read after write: the read waits for the write. */
    void* nrl = send_fd(ns_read_left, 16);
    assert(issued == 17 && issue_fd[17] == 16 && done[16]);
    /* Same namespace again: the next write waits for the read. */
    void* nw2 = send_fd(ns_write, 16);
    assert(issued == 18 && issue_fd[18] == 16 && done[17]);
    /* Another namespace: the read does not wait for the pending write. */
    void* nrr = send_fd(ns_read_right, 16);
    assert(issued == 19 && issue_fd[19] == 16 && !done[18]);
    /* Nor does an unnamed key with the same value. */
    void* unnamed = send_fd(read, 16);
    assert(issued == 20 && issue_fd[20] == 16 && !done[18]);
    finish(nw1, 16);
    finish(nrl, 17);
    finish(nw2, 18);
    finish(nrr, 19);
    finish(unnamed, 20);

    filc_async_stats stats;
    filc_async_get_stats(&stats);
    assert(stats.tasks_submitted == 21 && stats.tasks_completed == 21 &&
           stats.tasks_failed == 1 && issued == 20);
    puts("CHECK_DEPENDENCIES PASS");
    return 0;
}
