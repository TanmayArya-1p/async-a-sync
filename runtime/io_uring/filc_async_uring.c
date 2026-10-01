#include <errno.h>
#include <limits.h>

#include "fasync.h"
#include "fasync_internal.h"
#include "filc_async_alloc.h"
#include "filc_async_runtime.h"

/* The io_uring runtime, runtime=io_uring: implements the runtime side of
 * filc_async_runtime.h on the fasync_* request layer. It knows the ops
 * pread, pwrite, openat, fsync and close with their standard syscall
 * argument order, and op=ignore, a test-only op that completes with
 * -EOPNOTSUPP when polled. Submit runs the function's body, so a program can
 * instrument its calls, then queues the request.
 *
 * Requests are held in a table of FASYNC_MAX_INFLIGHT slots. A program that
 * only touches its buffers never polls, so when the table is full submit
 * completes the finished calls itself, or waits for the oldest. */

enum uring_op {
    URING_OP_UNKNOWN,
    URING_OP_READ,
    URING_OP_WRITE,
    URING_OP_FSYNC,
    URING_OP_CLOSE,
    URING_OP_OPENAT,
    URING_OP_IGNORE
};

/* The runtime's state for one call, in the task's runtime-data slot. */
struct uring_call {
    void* task;
    fasync_id request;       /* 0 for op=ignore */
    enum uring_op op;
    bool explicit_read;      /* a fasync_pread; its caller collects the result */
    bool finished;           /* completion reported */
    struct uring_call* newer; /* calls with a request in flight, oldest first */
    struct uring_call* older;
};

static struct uring_call* g_newest;
static struct uring_call* g_oldest;

static bool streq(const char* a, const char* b)
{
    if (!a || !b)
        return a == b;
    while (*a && *b) {
        if (*a != *b)
            return false;
        ++a;
        ++b;
    }
    return *a == *b;
}

// Reads the `op=<name>` token out of the null-terminated option array the
// pass copied into meta->opts.
static enum uring_op op_from(const char* const* opts)
{
    if (!opts)
        return URING_OP_UNKNOWN;
    for (size_t i = 0; opts[i]; ++i) {
        const char* s = opts[i];
        if (s[0] != 'o' || s[1] != 'p' || s[2] != '=')
            continue;
        const char* v = s + 3;
        if (streq(v, "pread"))
            return URING_OP_READ;
        if (streq(v, "pwrite"))
            return URING_OP_WRITE;
        if (streq(v, "fsync"))
            return URING_OP_FSYNC;
        if (streq(v, "close"))
            return URING_OP_CLOSE;
        if (streq(v, "openat"))
            return URING_OP_OPENAT;
        if (streq(v, "ignore"))
            return URING_OP_IGNORE;
        return URING_OP_UNKNOWN;
    }
    return URING_OP_UNKNOWN;
}

static bool is_output_kind(uint32_t kind)
{
    return kind == FILC_ASYNC_ARG_BUFFER_OUT ||
           kind == FILC_ASYNC_ARG_PENDING;
}

static bool is_input_kind(uint32_t kind)
{
    return kind == FILC_ASYNC_ARG_BUFFER_IN ||
           kind == FILC_ASYNC_ARG_PENDING;
}

static bool completion_args_ok(const filc_async_meta* m, unsigned required)
{
    if (m->nargs < required)
        return false;
    for (unsigned i = required; i < m->nargs; ++i)
        if (!is_output_kind(m->args[i].kind))
            return false;
    return true;
}

/* Whether the argument kinds match the op's syscall. Every op takes the
 * descriptor as argument 0, an unannotated integer. */
static bool shape_ok(const filc_async_meta* m, enum uring_op op)
{
    if (op == URING_OP_IGNORE)
        return true;
    if (!m->nargs || m->args[0].kind != FILC_ASYNC_ARG_IGNORED)
        return false;
    switch (op) {
    case URING_OP_READ:
        return completion_args_ok(m, 4) && is_output_kind(m->args[1].kind) &&
               m->args[2].kind == FILC_ASYNC_ARG_IGNORED &&
               m->args[3].kind == FILC_ASYNC_ARG_IGNORED;
    case URING_OP_WRITE:
    case URING_OP_OPENAT:
        return completion_args_ok(m, 4) && is_input_kind(m->args[1].kind) &&
               m->args[2].kind == FILC_ASYNC_ARG_IGNORED &&
               m->args[3].kind == FILC_ASYNC_ARG_IGNORED;
    case URING_OP_FSYNC:
    case URING_OP_CLOSE:
        return completion_args_ok(m, 1);
    default:
        return false;
    }
}

static bool uring_validate(const filc_async_meta* meta)
{
    if (!meta)
        return false;
    enum uring_op op = op_from(meta->opts);
    if (op == URING_OP_UNKNOWN)
        return false;
    return shape_ok(meta, op);
}

/* The staged array has one Fil-C pointer-sized cell per argument. Scalars
 * occupy its low word; pointer cells retain their capabilities. */
typedef struct {
    union {
        void* ptr;
        uint64_t word;
    } value;
    uint64_t capability;
} staged_arg;
_Static_assert(sizeof(staged_arg) == 16, "annotated argument slot must be 16 bytes");

static uint64_t arg_word(const staged_arg* args, size_t index)
{
    return args[index].value.word;
}

static void* arg_ptr(const staged_arg* args, size_t index)
{
    return args[index].value.ptr;
}

static fasync_id dispatch(enum uring_op op, const staged_arg* args)
{
    int fd = (int)arg_word(args, 0);
    switch (op) {
    case URING_OP_READ:
        return fasync_do_pread(fd, arg_ptr(args, 1), (size_t)arg_word(args, 2),
                               (unsigned long)arg_word(args, 3));
    case URING_OP_WRITE:
        return fasync_do_pwrite(fd, arg_ptr(args, 1), (size_t)arg_word(args, 2),
                                (unsigned long)arg_word(args, 3));
    case URING_OP_OPENAT:
        return fasync_do_openat(fd, (const char*)arg_ptr(args, 1),
                                (int)arg_word(args, 2), (int)arg_word(args, 3));
    case URING_OP_FSYNC:
        return fasync_fsync(fd);
    case URING_OP_CLOSE:
        return fasync_close(fd);
    default:
        return 0;
    }
}

static void list_add(struct uring_call* c)
{
    c->newer = NULL;
    c->older = g_newest;
    if (g_newest)
        g_newest->newer = c;
    else
        g_oldest = c;
    g_newest = c;
}

static void list_remove(struct uring_call* c)
{
    if (c->newer)
        c->newer->older = c->older;
    else
        g_newest = c->older;
    if (c->older)
        c->older->newer = c->newer;
    else
        g_oldest = c->newer;
}

/* Collects the request's result, which frees its slot, and reports it. An
 * explicit read keeps its slot for the caller's fasync_result. Called with
 * the runtime's lock held. */
static void finish(struct uring_call* c)
{
    if (c->finished)
        return;
    c->finished = true;
    if (c->explicit_read) {
        struct fasync_req_shared* r = fasync_req_lookup(c->request);
        filc_async_complete(c->task, r ? fasync_req_wait(r) : -EINVAL);
        return;
    }
    long result = c->op == URING_OP_IGNORE ? -EOPNOTSUPP
                                           : fasync_result(c->request);
    if (c->request)
        list_remove(c);
    filc_async_complete(c->task, result);
}

void fasync_track_read(fasync_id id, void* buf)
{
    struct fasync_req_shared* r = fasync_req_lookup(id);
    if (!r)
        return;
    struct uring_call* c = (struct uring_call*)filc_async_alloc(sizeof *c, 16);
    if (!c)
        filc_async_fatal("fasync_pread: out of memory");
    c->task = filc_async_task_new(&filc_async_runtime_io_uring);
    c->request = id;
    c->op = URING_OP_READ;
    c->explicit_read = true;
    *filc_async_task_runtime_data(c->task) = c;
    r->task = c->task;
    filc_async_mark_shared(c->task, buf);
}

/* Frees request slots when the table is full: completes the calls whose
 * requests have finished, oldest first, stopping at the first one still
 * running; if that is the oldest, waits for it. Returns false when no call
 * of this runtime holds a slot. */
static bool reclaim(void)
{
    bool reclaimed = false;
    for (struct uring_call* c = g_oldest; c; c = c->newer) {
        if (!fasync_ready(c->request)) {
            if (!reclaimed) {
                fasync_submit();
                finish(c);
            }
            return true;
        }
        finish(c);
        reclaimed = true;
    }
    return reclaimed;
}

/* Queues `c`'s request. Called with the runtime's lock held. */
static void queue(struct uring_call* c, const filc_async_meta* meta,
                  const staged_arg* args)
{
    if (c->op == URING_OP_IGNORE) {
        /* completes when polled, so its marks can be observed */
        *filc_async_task_runtime_data(c->task) = c;
        return;
    }
    if (!shape_ok(meta, c->op)) {
        filc_async_complete(c->task, -EINVAL);
        return;
    }
    if ((c->op == URING_OP_READ || c->op == URING_OP_WRITE) &&
        arg_word(args, 2) > UINT_MAX) {
        filc_async_complete(c->task, -EOVERFLOW);
        return;
    }
    for (;;) {
        errno = 0;
        c->request = dispatch(c->op, args);
        if (c->request)
            break;
        if (errno == EAGAIN && reclaim())
            continue;
        filc_async_complete(c->task, errno ? -errno : -EIO);
        return;
    }
    list_add(c);
    /* published last, so a poll from another thread never sees a call
     * without its request */
    *filc_async_task_runtime_data(c->task) = c;
}

static void uring_submit(void* task, const filc_async_meta* meta,
                         filc_async_run_fn run, void* staged_args, size_t nargs)
{
    if (!task || !meta || !staged_args || nargs != meta->nargs)
        filc_async_fatal("io_uring runtime: bad submit");

    /* The body and any wait for a buffer the kernel will read run without the
     * runtime's lock: both may wait for other calls. */
    filc_async_run(task, run, staged_args);

    struct uring_call* c = (struct uring_call*)filc_async_alloc(sizeof *c, 16);
    if (!c)
        filc_async_fatal("io_uring runtime: out of memory");
    c->task = task;
    c->op = op_from(meta->opts);

    const staged_arg* args = (const staged_arg*)staged_args;
    if ((c->op == URING_OP_WRITE || c->op == URING_OP_OPENAT) &&
        shape_ok(meta, c->op))
        filc_async_wait_buffer(task, arg_ptr(args, 1));

    fasync_lock();
    queue(c, meta, args);
    fasync_unlock();
}

/* Requests are queued until something needs a result, so the whole batch
 * goes to the kernel in one entry: CHECK never sends the queue, PROGRESS
 * sends it so a polling loop moves on, and BLOCK sends it and waits. */
static bool uring_poll(void* task, enum filc_async_poll_mode mode)
{
    fasync_lock();
    bool done = false;
    struct uring_call* c = (struct uring_call*)*filc_async_task_runtime_data(task);
    if (!c) {
        /* not submitted yet, or completed at submission */
    } else if (c->finished) {
        done = true;
    } else if (c->op == URING_OP_IGNORE) {
        /* completes when polled for real, so its marks can be observed */
        if (mode != FILC_ASYNC_POLL_CHECK) {
            finish(c);
            done = true;
        }
    } else {
        bool ready = fasync_ready(c->request);
        if (!ready && mode != FILC_ASYNC_POLL_CHECK) {
            if (fasync_submit() < 0)
                filc_async_fatal("io_uring submission failed");
            ready = mode == FILC_ASYNC_POLL_BLOCK || fasync_ready(c->request);
        }
        if (ready) {
            finish(c);
            done = true;
        }
    }
    fasync_unlock();
    return done;
}

FILC_ASYNC_RUNTIME(io_uring, uring_submit, uring_poll, uring_validate);
