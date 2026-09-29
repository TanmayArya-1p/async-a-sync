/* Drive the io_uring runtime directly, without the annotation compiler pass:
 * the calls a pass-emitted stub would make, by hand. */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "fasync.h"
#include "filc_async.h"
#include "filc_async_alloc.h"
#include "filc_async_runtime.h"

typedef struct {
    union {
        void* ptr;
        uint64_t word;
    } value;
    uint64_t capability;
} staged_arg;
_Static_assert(sizeof(staged_arg) == 16, "annotated argument slot must be 16 bytes");

static const char* const open_opts[] = { "op=openat", NULL };
static const char* const read_opts[] = { "op=pread", NULL };
static const char* const write_opts[] = { "op=pwrite", NULL };
static const char* const sync_opts[] = { "op=fsync", NULL };
static const char* const close_opts[] = { "op=close", NULL };

static filc_async_meta* make_meta(const char* name, const char* const* opts,
                                  unsigned nargs, unsigned buffer_kind)
{
    filc_async_meta* m = filc_async_alloc(
        sizeof(*m) + nargs * sizeof(m->args[0]), 16);
    if (!m)
        return NULL;
    m->name = name;
    m->nargs = nargs;
    m->noped_args = nargs == 1 ? 0 : 1;
    m->result = FILC_ASYNC_RESULT_PTR;
    m->opts = opts;
    m->runtime = &filc_async_runtime_io_uring;
    m->args[0].kind = FILC_ASYNC_ARG_IGNORED; /* the descriptor */
    if (nargs > 1)
        m->args[1].kind = buffer_kind;
    return m;
}

static staged_arg* make_args(unsigned nargs)
{
    return filc_async_alloc(nargs * sizeof(staged_arg), 16);
}

/* What a stub does: start a task, mark its output buffer, submit it. */
static void* start(const filc_async_meta* m, staged_arg* a, void* out)
{
    void* task = filc_async_begin(m, a);
    filc_async_mark_pending(task, out);
    filc_async_submit(task, m, NULL, a, m->nargs);
    return task;
}

static long finish(void* pending, unsigned char* state)
{
    struct filc_async_result_s r = { 0 };
    r.pending = pending;
    if (!filc_async_poll(&r))
        filc_async_wait(&r);
    *state = r.state;
    return r.result;
}

int main(int argc, char** argv)
{
    const char* dir = argc > 1 ? argv[1] : "/tmp";
    char path[512];
    if (snprintf(path, sizeof path, "%s/t_backend_io_uring_%ld.dat",
                 dir, (long)getpid()) >= (int)sizeof path)
        return 1;

    int seed = open(path, O_CREAT | O_TRUNC | O_RDWR, 0600);
    if (seed < 0)
        return 2;
    if (write(seed, "initial!", 8) != 8 || close(seed) != 0)
        return 3;

    filc_async_meta* open_meta = make_meta("open", open_opts, 4, FILC_ASYNC_ARG_BUFFER_IN);
    filc_async_meta* read_meta = make_meta("read", read_opts, 4, FILC_ASYNC_ARG_BUFFER_OUT);
    filc_async_meta* write_meta = make_meta("write", write_opts, 4, FILC_ASYNC_ARG_BUFFER_IN);
    filc_async_meta* sync_meta = make_meta("sync", sync_opts, 1, 0);
    filc_async_meta* close_meta = make_meta("close", close_opts, 1, 0);
    if (!open_meta || !read_meta || !write_meta || !sync_meta || !close_meta)
        return 4;
    const filc_async_meta* table[] = {
        open_meta, read_meta, write_meta, sync_meta, close_meta, NULL
    };
    filc_async_validate_table(table);

    staged_arg* a = make_args(4);
    if (!a)
        return 5;
    a[0].value.word = (uint64_t)(unsigned)AT_FDCWD;
    a[1].value.ptr = path;
    a[2].value.word = O_RDWR;
    a[3].value.word = 0;
    unsigned char state;
    int fd = (int)finish(start(open_meta, a, NULL), &state);
    if (state != 0 || fd < 0)
        return 6;

    char* buf = malloc(16);
    if (!buf)
        return 7;
    memset(buf, 0, 16);
    a = make_args(4);
    if (!a)
        return 8;
    a[0].value.word = (unsigned)fd;
    a[1].value.ptr = buf;
    a[2].value.word = 8;
    a[3].value.word = 0;
    void* read_task = start(read_meta, a, buf);
    if (!filc_async_is_pending(buf) || finish(read_task, &state) != 8 ||
        state != 0 || strcmp(buf, "initial!") != 0 || filc_async_is_pending(buf))
        return 9;

    a = make_args(4);
    if (!a)
        return 10;
    a[0].value.word = (unsigned)fd;
    a[1].value.ptr = (void*)"updated!";
    a[2].value.word = 8;
    a[3].value.word = 0;
    if (finish(start(write_meta, a, NULL), &state) != 8 || state != 0)
        return 11;

    a = make_args(1);
    if (!a)
        return 12;
    a[0].value.word = (unsigned)fd;
    if (finish(start(sync_meta, a, NULL), &state) != 0 || state != 0)
        return 13;

    memset(buf, 0, 16);
    a = make_args(4);
    if (!a)
        return 14;
    a[0].value.word = (unsigned)fd;
    a[1].value.ptr = buf;
    a[2].value.word = 8;
    a[3].value.word = 0;
    if (finish(start(read_meta, a, buf), &state) != 8 ||
        state != 0 || strcmp(buf, "updated!") != 0)
        return 15;

    a = make_args(1);
    if (!a)
        return 16;
    a[0].value.word = (unsigned)fd;
    if (finish(start(close_meta, a, NULL), &state) != 0 || state != 0)
        return 17;

    a = make_args(4);
    if (!a)
        return 18;
    a[0].value.word = (unsigned)-1;
    a[1].value.ptr = buf;
    a[2].value.word = 1;
    a[3].value.word = 0;
    if (finish(start(read_meta, a, buf), &state) != -EBADF ||
        state != 2 || filc_async_is_pending(buf))
        return 19;

    filc_async_stats stats;
    filc_async_get_stats(&stats);
    struct fasync_stats uring;
    fasync_get_stats(&uring);
    int ok = stats.tasks_submitted == 7 && stats.tasks_completed == 7 &&
             stats.tasks_failed == 1 && uring.sqes_queued >= 6;
    free(buf);
    unlink(path);
    printf("T_BACKEND_IO_URING %s (tasks=%lu sqes=%lu)\n",
           ok ? "PASS" : "FAIL", stats.tasks_completed, uring.sqes_queued);
    return ok ? 0 : 20;
}
