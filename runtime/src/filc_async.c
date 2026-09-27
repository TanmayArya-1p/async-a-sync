#include <errno.h>
#include <limits.h>
#include <pizlonated_syscalls.h>
#include <stdfil.h>

#include "fasync.h"
#include "fasync_shared.h"
#include "filc_async.h"
#include "filc_async_alloc.h"

/* The compiler's annotated-call ABI, backed by the same io_uring requests as
 * fasync_*. The annotated body is retained for linking but is not executed.
 * Tasks and staged arguments live in the allocator arena; the task list also
 * serves as a GC root for every in-flight pointer argument. */

struct filc_async_task;
static struct filc_async_task* g_tasks;

// pending-buffer registry. mark_pending/mark_resolved flip marks; is_pending
// tests range overlap so aliases of a marked buffer probe true. a mark holds
// the real buffer pointer (so resolve keeps its capability); bounds are derived
// via zgetlower/zgetupper. re-marking resolves the prior mark first.
#define FASYNC_PENDING_REGISTRY_CAPACITY FASYNC_MAX_INFLIGHT

typedef struct {
    void* buf;
    struct filc_async_task* owner;
} filc_async_pending_mark;

static filc_async_pending_mark g_pending[FASYNC_PENDING_REGISTRY_CAPACITY];
static size_t g_npending;

// mark_pending bumped once per stale mark it resolved before re-marking.
static unsigned long g_pending_resolves;

static unsigned long g_submitted;
static unsigned long g_completed;
static unsigned long g_failed;

struct filc_async_task {
    struct filc_async_task* next;
    void* impl;
    void* opts;
    const filc_async_meta* meta;
    void* staged_args;      /* keeps staged pointer capabilities alive */
    unsigned char state;    /* 0 done, 1 pending, 2 failed */
    unsigned char resolved; /* auto-resolve already ran for this task */
    fasync_id request;
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

static bool shape_ok(const filc_async_meta* m, enum fasync_op op);

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

/* noped_args counts fd=/bin=/bout=/buf= options. Check that count and the
 * argument shape expected by each supported operation. The runtime owns the
 * operation list; the pass does not validate op=. */
static bool arg_kinds_ok(const filc_async_meta* m)
{
    if (!m)
        return false;
    if (m->noped_args > m->nargs)
        return false;
    enum fasync_op op = opcode_from(m->opts);
    if (op == FASYNC_OP_UNKNOWN)
        return false;
    if (op != FASYNC_OP_IGNORE && m->noped_args < 1)
        return false;
    return shape_ok(m, op);
}

static bool default_validator(const filc_async_meta* m)
{
    return arg_kinds_ok(m);
}

static filc_async_register_fn g_register_fn;

static struct filc_async_task* find_task(const void* pending)
{
    for (struct filc_async_task* t = g_tasks; t; t = t->next) {
        if ((const void*)t == pending)
            return t;
    }
    return NULL;
}

/* The staged array has one Fil-C pointer-sized cell per argument. Scalars
 * occupy its low word; pointer cells retain their capabilities. */
typedef union {
    void* ptr;
    uint64_t word;
} filc_async_arg;

static uint64_t arg_word(const filc_async_arg* args, size_t index)
{
    return args[index].word;
}

static void* arg_ptr(const filc_async_arg* args, size_t index)
{
    return args[index].ptr;
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

static bool shape_ok(const filc_async_meta* m, enum fasync_op op)
{
    if (op == FASYNC_OP_IGNORE)
        return true;
    if (!m->nargs)
        return false;
    if (m->args[0].kind != FILC_ASYNC_ARG_FD)
        return false;
    switch (op) {
    case FASYNC_OP_READ:
        return m->nargs == 4 && is_output_kind(m->args[1].kind) &&
               m->args[2].kind == FILC_ASYNC_ARG_IGNORED &&
               m->args[3].kind == FILC_ASYNC_ARG_IGNORED;
    case FASYNC_OP_WRITE:
        return m->nargs == 4 && is_input_kind(m->args[1].kind) &&
               m->args[2].kind == FILC_ASYNC_ARG_IGNORED &&
               m->args[3].kind == FILC_ASYNC_ARG_IGNORED;
    case FASYNC_OP_OPENAT:
        return m->nargs == 4 && is_input_kind(m->args[1].kind) &&
               m->args[2].kind == FILC_ASYNC_ARG_IGNORED &&
               m->args[3].kind == FILC_ASYNC_ARG_IGNORED;
    case FASYNC_OP_FSYNC:
    case FASYNC_OP_CLOSE:
        return m->nargs == 1;
    default:
        return false;
    }
}

static fasync_id dispatch_request(enum fasync_op op,
                                  const filc_async_arg* args)
{
    int fd = (int)arg_word(args, 0);
    switch (op) {
    case FASYNC_OP_READ:
        return fasync_pread(fd, arg_ptr(args, 1), (size_t)arg_word(args, 2),
                            (unsigned long)arg_word(args, 3));
    case FASYNC_OP_WRITE:
        return fasync_pwrite(fd, arg_ptr(args, 1), (size_t)arg_word(args, 2),
                             (unsigned long)arg_word(args, 3));
    case FASYNC_OP_OPENAT:
        return fasync_openat(fd, (const char*)arg_ptr(args, 1),
                             (int)arg_word(args, 2), (int)arg_word(args, 3));
    case FASYNC_OP_FSYNC:
        return fasync_fsync(fd);
    case FASYNC_OP_CLOSE:
        return fasync_close(fd);
    default:
        return 0;
    }
}

static void auto_resolve_buffers(struct filc_async_task* t);

static void complete_task(struct filc_async_task* t, long result)
{
    if (t->state != 1)
        return;
    t->request = 0;
    t->result = result;
    t->state = result < 0 ? 2 : 0;
    ++g_completed;
    if (result < 0)
        ++g_failed;
    auto_resolve_buffers(t);
}

static void refresh_task(struct filc_async_task* t, bool wait)
{
    if (t->state != 1)
        return;
    if (!t->request) {
        complete_task(t, t->result);
        return;
    }
    if (wait && fasync_submit() < 0)
        filc_async_fatal("io_uring submission failed");
    if (!wait && !fasync_ready(t->request))
        return;
    complete_task(t, fasync_result(t->request));
}

void* filc_async_submit(const filc_async_meta* meta, void* impl, void* opts,
                        void* staged_args, size_t nargs)
{
    if (!meta || !staged_args || nargs != meta->nargs)
        filc_async_fatal("filc_async_submit: bad call");

    struct filc_async_task* t = (struct filc_async_task*)filc_async_alloc(sizeof *t, 16);
    if (!t)
        filc_async_fatal("filc_async_submit: out of memory");

    t->impl = impl;
    t->opts = opts;
    t->meta = meta;
    t->staged_args = staged_args;
    t->resolved = 0;
    t->state = 1;
    t->next = g_tasks;
    g_tasks = t;
    ++g_submitted;

    // The compiler marks output buffers just before calling submit. Attach
    // those marks to this request so re-marking can wait for the right owner.
    const filc_async_arg* args = (const filc_async_arg*)staged_args;
    for (size_t i = 0; i < nargs; ++i) {
        uint32_t kind = meta->args[i].kind;
        if (kind != FILC_ASYNC_ARG_BUFFER_OUT && kind != FILC_ASYNC_ARG_PENDING)
            continue;
        void* buf = arg_ptr(args, i);
        for (size_t j = 0; j < g_npending; ++j)
            if (g_pending[j].buf == buf && !g_pending[j].owner)
                g_pending[j].owner = t;
    }

    enum fasync_op op = opcode_from(meta->opts);
    if (op == FASYNC_OP_IGNORE) {
        // Preserve the pass's pending marks until poll/wait observes the task.
        t->result = -EOPNOTSUPP;
    } else if (!shape_ok(meta, op)) {
        complete_task(t, -EINVAL);
    } else if ((op == FASYNC_OP_READ || op == FASYNC_OP_WRITE) &&
               arg_word(args, 2) > UINT_MAX) {
        complete_task(t, -EOVERFLOW);
    } else {
        errno = 0;
        t->request = dispatch_request(op, args);
        if (!t->request)
            complete_task(t, errno ? -errno : -EIO);
    }

    return (void*)t;
}

// On completion (poll/wait), clear the pending marks the pass set for this
// task's producing buffers. Idempotent per task.
static void auto_resolve_buffers(struct filc_async_task* t)
{
    if (t->resolved)
        return;
    t->resolved = 1;
    for (size_t j = 0; j < g_npending;)
        if (g_pending[j].owner == t) {
            g_pending[j] = g_pending[--g_npending];
        } else {
            ++j;
        }
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
    if (t->state == 1 && t->request && fasync_submit() < 0)
        filc_async_fatal("io_uring submission failed");
    refresh_task(t, false);
    result_fill(out, t);
    return t->state != 1;
}

void filc_async_wait(struct filc_async_result_s* out)
{
    if (!out || !out->pending)
        return;
    struct filc_async_task* t = find_task(out->pending);
    if (!t)
        return;
    refresh_task(t, true);
    result_fill(out, t);
}

void filc_async_mark_pending(void* buf)
{
    if (!buf)
        return;
    uintptr_t lower = (uintptr_t)zgetlower(buf);
    uintptr_t upper = (uintptr_t)zgetupper(buf);

    // A range already claimed by an older op must finish first. Completing
    // its task also retires the io_uring request and clears all its marks.
    for (size_t i = 0; i < g_npending;) {
        uintptr_t ilower = (uintptr_t)zgetlower(g_pending[i].buf);
        uintptr_t iupper = (uintptr_t)zgetupper(g_pending[i].buf);
        if (lower < iupper && ilower < upper) {
            if (g_pending[i].owner) {
                refresh_task(g_pending[i].owner, true);
            } else {
                // Standalone marks may describe an explicit fasync request.
                fasync_resolve_pending(g_pending[i].buf, 1);
                g_pending[i] = g_pending[--g_npending];
            }
            g_pending_resolves++;
            i = 0;
        } else {
            i++;
        }
    }

    if (g_npending >= FASYNC_PENDING_REGISTRY_CAPACITY)
        filc_async_fatal("too many pending buffers");
    g_pending[g_npending].buf = buf;
    g_pending[g_npending].owner = NULL;
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
    // The compiler's access hook may have reaped a CQE without entering this
    // API. Observe those completions before reporting registry state.
    for (size_t i = 0; i < g_npending;) {
        struct filc_async_task* owner = g_pending[i].owner;
        if (owner && owner->request && owner->state == 1)
            refresh_task(owner, false);
        if (i < g_npending && g_pending[i].owner == owner)
            ++i;
    }
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
    *executes_bodies = 0; /* the io_uring backend executes the syscall */
}

void filc_async_get_stats(filc_async_stats* out)
{
    if (!out)
        return;
    for (struct filc_async_task* t = g_tasks; t; t = t->next)
        if (t->request && t->state == 1)
            refresh_task(t, false);
    *out = (filc_async_stats){ 0 };
    out->tasks_submitted = g_submitted;
    out->tasks_completed = g_completed;
    out->tasks_failed = g_failed;
    out->pending_resolves = g_pending_resolves;
    struct fasync_stats stats;
    fasync_get_stats(&stats);
    out->sqes_queued = stats.sqes_queued;
    out->kernel_submit_entries = stats.kernel_submit_entries;
    out->kernel_wait_entries = stats.kernel_wait_entries;
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
