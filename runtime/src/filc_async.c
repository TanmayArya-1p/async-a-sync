#include <errno.h>
#include <pizlonated_syscalls.h>

#include "filc_async.h"
#include "filc_async_alloc.h"

/* Minimal link-level filc_async runtime: lets every compiler-emitted symbol
 * link and run. submit immediately fails each task with -EOPNOTSUPP, poll/wait
 * resolve it from a small identity registry, and startup validation accepts
 * the op set the compiler bakes into the descriptors. This is the seed for
 * the io_uring backend (a later branch); nothing here touches fasync_*.
 */

#define FASYNC_TASK_REGISTRY_CAPACITY 64

static void* g_tasks[FASYNC_TASK_REGISTRY_CAPACITY];
static size_t g_ntasks;

// Stat counters. Every task resolves instantly, so it completes and fails at
// submit time.
static unsigned long g_submitted;
static unsigned long g_completed;
static unsigned long g_failed;

struct filc_async_task {
    void* impl;
    void* opts;
    const filc_async_meta* meta;
    unsigned char state; /* 0 done, 1 pending, 2 failed */
    long result;
};

// Op set: pread/pwrite -> READ/WRITE, openat, fsync, close, and `ignore`
// (valid, never executed).
enum fasync_op {
    FASYNC_OP_UNKNOWN,
    FASYNC_OP_READ,
    FASYNC_OP_WRITE,
    FASYNC_OP_FSYNC,
    FASYNC_OP_CLOSE,
    FASYNC_OP_OPENAT,
    FASYNC_OP_IGNORE
};

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

// Read the `op=<name>` token out of the null-terminated option-string array
// the pass copied into meta->opts. Unknown -> FASYNC_OP_UNKNOWN.
static enum fasync_op opcode_from(const char* const* opts)
{
    if (!opts)
        return FASYNC_OP_UNKNOWN;
    for (size_t i = 0; opts[i]; ++i) {
        const char* s = opts[i];
        if (s[0] != 'o' || s[1] != 'p' || s[2] != '=')
            continue;
        const char* v = s + 3;
        if (streq(v, "pread"))
            return FASYNC_OP_READ;
        if (streq(v, "pwrite"))
            return FASYNC_OP_WRITE;
        if (streq(v, "fsync"))
            return FASYNC_OP_FSYNC;
        if (streq(v, "close"))
            return FASYNC_OP_CLOSE;
        if (streq(v, "openat"))
            return FASYNC_OP_OPENAT;
        if (streq(v, "ignore"))
            return FASYNC_OP_IGNORE;
        return FASYNC_OP_UNKNOWN;
    }
    return FASYNC_OP_UNKNOWN;
}

/* arg_kinds_ok: noped_args is the number of fd=/buf= options the pass
 * counted; the five real ops consume at least one of them. `ignore` is
 * exempt (noped_args may be 0). An unknown op is invalid here: the runtime
 * is the authority for the op set (the pass does not validate it). */
static bool arg_kinds_ok(const filc_async_meta* m)
{
    if (!m)
        return false;
    if (m->noped_args > m->nargs)
        return false;
    if (m->nargs != 0) {
        // A descriptor for a real function must have a real args[] tail.
        const void* arg_array = m->args;
        if (arg_array == NULL)
            return false;
    }
    enum fasync_op op = opcode_from(m->opts);
    if (op == FASYNC_OP_UNKNOWN)
        return false;
    if (op == FASYNC_OP_IGNORE)
        return true; /* noped_args==0 is fine for ignore */
    return m->noped_args >= 1;
}

static bool default_validator(const filc_async_meta* m)
{
    return arg_kinds_ok(m);
}

static filc_async_register_fn g_register_fn;

static struct filc_async_task* find_task(const void* pending)
{
    for (size_t i = 0; i < g_ntasks; ++i) {
        struct filc_async_task* t = g_tasks[i];
        if ((const void*)t == pending)
            return t;
    }
    return NULL;
}

void* filc_async_submit(const filc_async_meta* meta, void* impl, void* opts,
                        void* staged_args, size_t nargs)
{
    if (!meta || !staged_args || nargs != meta->nargs)
        filc_async_fatal("filc_async_submit: bad call");

    // Staged args, impl, opts are owned by the runtime from here. This
    // runtime analyses none of them -- it resolves instantly regardless of
    // op -- but they are kept on the task record for a real backend to reap.
    struct filc_async_task* t = (struct filc_async_task*)filc_async_alloc(sizeof *t, 16);
    if (!t)
        filc_async_fatal("filc_async_submit: out of memory");

    t->impl = impl;
    t->opts = opts;
    t->meta = meta;
    // Immediate-fail placeholder; the io_uring backend replaces the dispatch.
    t->state = 2;
    t->result = -EOPNOTSUPP;

    if (g_ntasks >= FASYNC_TASK_REGISTRY_CAPACITY)
        filc_async_fatal("filc_async_submit: too many pending tasks");
    g_tasks[g_ntasks++] = t;
    ++g_submitted;
    ++g_completed;
    ++g_failed;

    return (void*)t;
}

static void result_fill(struct filc_async_result_s* out, struct filc_async_task* t)
{
    out->result = t->result;
    out->state = t->state;
}

bool filc_async_poll(struct filc_async_result_s* out)
{
    if (!out || !out->pending)
        return false;
    struct filc_async_task* t = find_task(out->pending);
    if (!t)
        return false;
    result_fill(out, t);
    return true; /* this runtime's tasks are always already done */
}

void filc_async_wait(struct filc_async_result_s* out)
{
    if (!out || !out->pending)
        return;
    struct filc_async_task* t = find_task(out->pending);
    if (!t)
        return;
    result_fill(out, t);
}

void filc_async_capabilities(unsigned long* syscall_shaped, unsigned long* executes_bodies)
{
    *syscall_shaped = 1;
    *executes_bodies = 0; /* nothing executes bodies until the real backend */
}

void filc_async_get_stats(filc_async_stats* out)
{
    if (!out)
        return;
    *out = (filc_async_stats){ 0 };
    out->tasks_submitted = g_submitted;
    out->tasks_completed = g_completed;
    out->tasks_failed = g_failed;
}

void filc_async_set_register_fn(filc_async_register_fn fn)
{
    g_register_fn = fn;
}

void filc_async_validate_table(const filc_async_meta* const* metas)
{
    filc_async_register_fn validator = g_register_fn ? g_register_fn : default_validator;
    for (const filc_async_meta* const* p = metas; p && *p; ++p) {
        const filc_async_meta* m = *p;
        if (!validator(m))
            filc_async_fatal("function cannot be registered on this runtime");
    }
}

void filc_async_fatal(const char* msg)
{
    // Runtime objects in libpizlo.a cannot reach pizlonated libc (fprintf/
    // abort): the driver links `-lc` BEFORE `-lpizlo`. Go through the zsys
    // native thunks our own archive exports instead, like fasync.c does.
    static const char prefix[] = "filc_async: fatal: ";
    zsys_write(2, prefix, sizeof(prefix) - 1);
    for (const char* p = msg; *p; ++p)
        zsys_write(2, p, 1);
    zsys_write(2, "\n", 1);
    zsys_abort();
    while (1) { }
}