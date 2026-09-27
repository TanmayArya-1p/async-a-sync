#include <errno.h>
#include <pizlonated_syscalls.h>
#include <stdfil.h>

#include "fasync.h"
#include "filc_async.h"
#include "filc_async_alloc.h"

/* Minimal link-level filc_async runtime: lets every compiler-emitted symbol
 * link and run. submit immediately fails each task with -EOPNOTSUPP, poll/wait
 * resolve it from a small identity registry, and startup validation accepts
 * the op set the compiler bakes into the descriptors. Tracks the pass-emitted
 * pending marks (mark_pending/mark_resolved/is_pending): marking a buffer
 * already claimed by an older op resolves that op through the generic
 * resolution gate first, and poll/wait auto-resolve the task's marked buffers
 * on completion. This is the seed for the real backend (a later branch).
 */

#define FASYNC_TASK_REGISTRY_CAPACITY 64

static void* g_tasks[FASYNC_TASK_REGISTRY_CAPACITY];
static size_t g_ntasks;

// pending-buffer registry. mark_pending/mark_resolved flip marks; is_pending
// tests range overlap so aliases of a marked buffer probe true. a mark holds
// the real buffer pointer (so resolve keeps its capability); bounds are derived
// via zgetlower/zgetupper. re-marking resolves the prior mark first.
#define FASYNC_PENDING_REGISTRY_CAPACITY 64

typedef struct {
    void* buf;
} filc_async_pending_mark;

static filc_async_pending_mark g_pending[FASYNC_PENDING_REGISTRY_CAPACITY];
static size_t g_npending;

// mark_pending bumped once per stale mark it resolved before re-marking.
static unsigned long g_pending_resolves;

// Stat counters. Every task resolves instantly, so it completes and fails at
// submit time.
static unsigned long g_submitted;
static unsigned long g_completed;
static unsigned long g_failed;

struct filc_async_task {
    void* impl;
    void* opts;
    const filc_async_meta* meta;
    void* staged_args;      /* pass-emitted arg array; auto-resolve reads it */
    unsigned char state;    /* 0 done, 1 pending, 2 failed */
    unsigned char resolved; /* auto-resolve already ran for this task */
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

/* arg_kinds_ok: noped_args is the number of fd=/bin=/bout=/buf= options the
 * pass counted; the five real ops consume at least one of them. `ignore` is
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

static struct filc_async_task* find_task(const void* pending, size_t* index)
{
    for (size_t i = 0; i < g_ntasks; ++i) {
        struct filc_async_task* t = g_tasks[i];
        if ((const void*)t == pending) {
            *index = i;
            return t;
        }
    }
    return NULL;
}

// A task whose completion has been delivered leaves the registry, so the
// registry bounds tasks in flight rather than tasks over the program's life.
static void retire_task(size_t index)
{
    g_tasks[index] = g_tasks[g_ntasks - 1];
    g_ntasks--;
}

void* filc_async_submit(const filc_async_meta* meta, void* impl, void* opts,
                        void* staged_args, size_t nargs)
{
    if (!meta || !staged_args || nargs != meta->nargs)
        filc_async_fatal("filc_async_submit: bad call");
    if (g_ntasks >= FASYNC_TASK_REGISTRY_CAPACITY)
        filc_async_fatal("filc_async_submit: too many pending tasks");

    // Staged args, impl, opts are owned by the runtime from here. This
    // runtime analyses none of them -- it resolves instantly regardless of
    // op -- but they are kept on the task record for a real backend to reap.
    struct filc_async_task* t = (struct filc_async_task*)filc_async_alloc(sizeof *t, 16);
    if (!t)
        filc_async_fatal("filc_async_submit: out of memory");

    t->impl = impl;
    t->opts = opts;
    t->meta = meta;
    t->staged_args = staged_args;
    t->resolved = 0;
    // Immediate-fail placeholder; the io_uring backend replaces the dispatch.
    t->state = 2;
    t->result = -EOPNOTSUPP;

    g_tasks[g_ntasks++] = t;
    ++g_submitted;
    ++g_completed;
    ++g_failed;

    return (void*)t;
}

// On completion (poll/wait), clear the pending marks the pass set for this
// task's producing buffers. Idempotent per task; address-based so it needs no
// capability (the staging holds intvals).
static void auto_resolve_buffers(struct filc_async_task* t)
{
    if (t->resolved)
        return;
    t->resolved = 1;
    for (size_t i = 0; i < t->meta->nargs; ++i) {
        uint32_t kind = t->meta->args[i].kind;
        if (kind != FILC_ASYNC_ARG_BUFFER_OUT && kind != FILC_ASYNC_ARG_PENDING)
            continue;
        uintptr_t addr = ((const uint64_t*)t->staged_args)[i * 2];
        for (size_t j = 0; j < g_npending; ++j)
            if ((uintptr_t)g_pending[j].buf == addr) {
                g_pending[j] = g_pending[g_npending - 1];
                g_npending--;
                break;
            }
    }
}

static void result_fill(struct filc_async_result_s* out, struct filc_async_task* t)
{
    auto_resolve_buffers(t);
    out->result = t->result;
    out->state = t->state;
}

bool filc_async_poll(struct filc_async_result_s* out)
{
    if (!out || !out->pending)
        return false;
    size_t index;
    struct filc_async_task* t = find_task(out->pending, &index);
    if (!t)
        return false;
    result_fill(out, t);
    retire_task(index);
    return true; /* this runtime's tasks are always already done */
}

void filc_async_wait(struct filc_async_result_s* out)
{
    if (!out || !out->pending)
        return;
    size_t index;
    struct filc_async_task* t = find_task(out->pending, &index);
    if (!t)
        return;
    result_fill(out, t);
    retire_task(index);
}

void filc_async_mark_pending(void* buf)
{
    if (!buf)
        return;
    uintptr_t lower = (uintptr_t)zgetlower(buf);
    uintptr_t upper = (uintptr_t)zgetupper(buf);

    // a range already claimed by an older op must finish first: wait on the
    // generic resolution gate, drop its stale mark, then requeue one fresh
    // entry for this op.
    for (size_t i = 0; i < g_npending;) {
        uintptr_t ilower = (uintptr_t)zgetlower(g_pending[i].buf);
        uintptr_t iupper = (uintptr_t)zgetupper(g_pending[i].buf);
        if (lower < iupper && ilower < upper) {
            // size 1 = resolve the prior op's buffer start, like the access
            // hook (filc_resolve_pending); keeps the pointer's capability.
            fasync_resolve_pending(g_pending[i].buf, 1);
            g_pending[i] = g_pending[g_npending - 1];
            g_npending--;
            g_pending_resolves++;
        } else {
            i++;
        }
    }

    if (g_npending >= FASYNC_PENDING_REGISTRY_CAPACITY)
        filc_async_fatal("too many pending buffers");
    g_pending[g_npending].buf = buf;
    g_npending++;
}

void filc_async_mark_resolved(void* buf)
{
    if (!buf)
        return;
    uintptr_t lower = (uintptr_t)zgetlower(buf);
    for (size_t i = 0; i < g_npending; ++i)
        if ((uintptr_t)zgetlower(g_pending[i].buf) == lower) {
            g_pending[i] = g_pending[g_npending - 1];
            g_npending--;
            return;
        }
}

bool filc_async_is_pending(const void* buf)
{
    if (!buf)
        return false;
    uintptr_t lower = (uintptr_t)zgetlower((void*)buf);
    uintptr_t upper = (uintptr_t)zgetupper((void*)buf);
    for (size_t i = 0; i < g_npending; ++i) {
        uintptr_t ilower = (uintptr_t)zgetlower(g_pending[i].buf);
        uintptr_t iupper = (uintptr_t)zgetupper(g_pending[i].buf);
        if (lower < iupper && ilower < upper)
            return true;
    }
    return false;
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
    out->pending_resolves = g_pending_resolves;
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