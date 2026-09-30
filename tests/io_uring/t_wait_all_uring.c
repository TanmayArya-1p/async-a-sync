#include <assert.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "fasync.h"
#include "filc_async.h"
#include "filc_async_runtime.h"
#include "filc_async_alloc.h"

#ifdef FASYNC_COMPILER_INSERTS_CHECKS
static unsigned body_calls;
#pragma clang attribute push(__attribute__((annotate("filc_async", "runtime=io_uring", "op=pread", "bout=data"))), apply_to=function)
__attribute__((noinline))
void* joined_read(int fd, void* data, size_t len, unsigned long offset, void* done)
{
    (void)fd; (void)data; (void)len; (void)offset; (void)done;
    ++body_calls;
    return NULL;
}
#pragma clang attribute pop
#pragma clang attribute push(__attribute__((annotate("filc_async", "runtime=io_uring", "op=pwrite", "bin=data"))), apply_to=function)
__attribute__((noinline))
void* joined_write(int fd, const void* data, size_t len, unsigned long offset, void* done)
{
    (void)fd; (void)data; (void)len; (void)offset; (void)done;
    ++body_calls;
    return NULL;
}
#pragma clang attribute pop
#else
// the same stub protocol, for testing with the stock compiler
static filc_async_meta* make_meta(const char* op, unsigned kind)
{
    filc_async_meta* m = calloc(1, sizeof *m + 5 * sizeof m->args[0]);
    const char** opts = calloc(2, sizeof *opts);
    assert(m && opts);
    opts[0] = op;
    m->name = op;
    m->nargs = 5;
    m->opts = opts;
    m->runtime = &filc_async_runtime_io_uring;
    m->args[1].kind = kind;
    m->args[4].kind = FILC_ASYNC_ARG_PENDING;
    return m;
}

typedef union { void* ptr; uint64_t word; char cell[16]; } arg;
static void* joined_call(const char* op, unsigned kind, int fd, void* data,
                         size_t len, unsigned long offset, void* done)
{
    filc_async_meta* m = make_meta(op, kind);
    const filc_async_meta* table[] = { m, NULL };
    filc_async_validate_table(table);
    arg* a = filc_async_alloc(5 * sizeof *a, 16);
    assert(a && sizeof *a == 16);
    a[0].word = (unsigned)fd;
    a[1].ptr = data;
    a[2].word = len;
    a[3].word = offset;
    a[4].ptr = done;
    void* task = filc_async_begin(m, a);
    if (kind == FILC_ASYNC_ARG_BUFFER_OUT)
        filc_async_mark_pending(task, data);
    filc_async_mark_pending(task, done);
    filc_async_submit(task, m, NULL, a, 5);
    return task;
}

static void* joined_read(int fd, void* data, size_t len,
                         unsigned long offset, void* done)
{
    return joined_call("op=pread", FILC_ASYNC_ARG_BUFFER_OUT,
                       fd, data, len, offset, done);
}

static void* joined_write(int fd, const void* data, size_t len,
                          unsigned long offset, void* done)
{
    return joined_call("op=pwrite", FILC_ASYNC_ARG_BUFFER_IN,
                       fd, (void*)data, len, offset, done);
}

static void validation_cases(void)
{
    filc_async_meta* m = make_meta("op=pread", FILC_ASYNC_ARG_BUFFER_OUT);
    assert(m->runtime->validate(m));
    m->args[4].kind = FILC_ASYNC_ARG_IGNORED;
    assert(!m->runtime->validate(m));
    m->args[4].kind = FILC_ASYNC_ARG_BUFFER_IN;
    assert(!m->runtime->validate(m));
    m->args[4].kind = FILC_ASYNC_ARG_BUFFER_OUT;
    assert(m->runtime->validate(m));
    m->nargs = 3;
    assert(!m->runtime->validate(m));
    const char* operations[] = { "op=pwrite", "op=openat", "op=fsync", "op=close" };
    for (unsigned i = 0; i < 4; ++i) {
        m = make_meta(operations[i], FILC_ASYNC_ARG_BUFFER_IN);
        unsigned first_extra = i < 2 ? 4 : 1;
        m->nargs = first_extra + 1;
        m->args[first_extra].kind = FILC_ASYNC_ARG_PENDING;
        assert(m->runtime->validate(m));
        m->args[first_extra].kind = FILC_ASYNC_ARG_IGNORED;
        assert(!m->runtime->validate(m));
        m->args[first_extra].kind = FILC_ASYNC_ARG_BUFFER_IN;
        assert(!m->runtime->validate(m));
        m->nargs = first_extra;
        if (first_extra == 1)
            assert(m->runtime->validate(m));
    }
}
#endif

int main(int argc, char** argv)
{
#ifndef FASYNC_COMPILER_INSERTS_CHECKS
    validation_cases();
#endif
    char path[512];
    assert(snprintf(path, sizeof path, "%s/wait-all-%ld.dat",
                    argc > 1 ? argv[1] : "/tmp", (long)getpid()) < (int)sizeof path);
    int fd = open(path, O_CREAT | O_EXCL | O_RDWR, 0600);
    assert(fd >= 0);
    assert(write(fd, "abcdefghijklmnopqrstuvwxyz1234", 30) == 30);
    void* tasks[30];
    const void* tokens[30];
    char* data[30];
    for (unsigned i = 0; i < 30; ++i) {
        tokens[i] = calloc(1, 1);
        data[i] = calloc(1, 1);
        assert(tokens[i] && data[i]);
        tasks[i] = joined_read(fd, data[i], 1, i, (void*)tokens[i]);
    }
    filc_async_stats before;
    filc_async_get_stats(&before);
    assert(before.tasks_submitted == 30 && before.tasks_completed == 0);
    void* group = filc_async_wait_all_array(tokens, 30);
    assert(filc_async_is_pending(group));
    void* writer = joined_write(fd, "!", 1, 0, group);
    filc_async_stats after;
    filc_async_get_stats(&after);
    assert(after.tasks_submitted == 31 && after.tasks_completed >= 30);
    assert(after.pending_resolves == 1);
    for (unsigned i = 0; i < 30; ++i) {
        struct filc_async_result_s r = { .pending = tasks[i] };
        filc_async_wait(&r);
        assert(r.state == 0 && r.result == 1);
        assert(*data[i] == "abcdefghijklmnopqrstuvwxyz1234"[i]);
    }
    struct filc_async_result_s r = { .pending = writer };
    filc_async_wait(&r);
    assert(r.state == 0 && r.result == 1);
    char byte;
    assert(pread(fd, &byte, 1, 0) == 1 && byte == '!');
    assert(!filc_async_is_pending(group));
    assert(!close(fd));
    assert(!unlink(path));
#ifdef FASYNC_COMPILER_INSERTS_CHECKS
    assert(body_calls == 31);
#endif
    puts("WAIT_ALL_URING PASS (30 reads queued before write handoff)");
    return 0;
}
