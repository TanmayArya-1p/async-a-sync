#include <errno.h>
#include <limits.h>
#include <pizlonated_syscalls.h>
#include <stdfil.h>

#include "fasync.h"
#include "fasync_internal.h"
#include "fasync_shared.h"
#include "filc_async.h"
#include "filc_async_alloc.h"

/* The compiler's annotated-call ABI, backed by the same io_uring requests as
 * fasync_*. The annotated body is retained for linking but is not executed.
 * Tasks and staged arguments live in the allocator arena.
 *
 * g_tasks lists the tasks still in flight, newest first; dependency ordering
 * and every scan for work only walk this list. A task moves to g_done when it
 * completes and leaves that list once poll/wait delivers its completion. A
 * program that only ever touches its buffers never delivers, so g_done can
 * grow, but nothing walks it except a poll/wait looking up its handle. */

struct filc_async_task;
static struct filc_async_task* g_tasks;
static struct filc_async_task* g_done;

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
    struct filc_async_task* next;      /* g_tasks, while in flight */
    struct filc_async_task* done_next; /* g_done, once complete */
    void* impl;
    void* opts;
    const filc_async_meta* meta;
    void* staged_args;      /* keeps staged pointer capabilities alive */
    unsigned char state;    /* 0 done, 1 pending, 2 failed */
    unsigned char started;  /* an SQE has been issued (or an error recorded) */
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
    for (size_t i = 0; i < m->nargs; ++i) {
        unsigned dep = m->args[i].dependency;
        unsigned mode = dep & ~FILC_ASYNC_DEP_POINTER;
        if (mode != FILC_ASYNC_DEP_NONE && mode != FILC_ASYNC_DEP_READ &&
            mode != FILC_ASYNC_DEP_WRITE)
            return false;
        if (dep & ~(FILC_ASYNC_DEP_POINTER | FILC_ASYNC_DEP_READ |
                    FILC_ASYNC_DEP_WRITE))
            return false;
        if (mode == FILC_ASYNC_DEP_NONE && dep != FILC_ASYNC_DEP_NONE)
            return false;
    }
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
        if ((const void*)t == pending) {
            return t;
        }
    }
    for (struct filc_async_task* t = g_done; t; t = t->done_next) {
        if ((const void*)t == pending) {
            return t;
        }
    }
    return NULL;
}

/* A task whose completion has been delivered leaves g_done; later poll/wait
 * calls on its handle find nothing and leave the caller's result alone. */
static void retire_task(struct filc_async_task* t)
{
    for (struct filc_async_task** link = &g_done; *link;
         link = &(*link)->done_next) {
        if (*link == t) {
            *link = t->done_next;
            break;
        }
    }
}

/* The staged array has one Fil-C pointer-sized cell per argument. Scalars
 * occupy its low word; pointer cells retain their capabilities. */
typedef struct {
    union {
        void* ptr;
        uint64_t word;
    } value;
    uint64_t capability;
} filc_async_arg;
_Static_assert(sizeof(filc_async_arg) == 16, "annotated argument slot must be 16 bytes");

static uint64_t arg_word(const filc_async_arg* args, size_t index)
{
    return args[index].value.word;
}

static void* arg_ptr(const filc_async_arg* args, size_t index)
{
    return args[index].value.ptr;
}

/* Dependency claims: submission order assigns the call order, and completion
 * retires a task's claims. The runtime is single-threaded (see
 * fasync_check_thread), so none of this state is locked. */

/* FIXME: scalar keys depend on the declared integer width.
 *
 * FilAsync stages an integer narrower than 64 bits zero-extended
 * (Builder.CreateZExt in FilAsync.cpp's call rewriting), and the scalar
 * comparison below compares the full 64-bit words. The same value declared
 * with different widths therefore does not always match. For example, a
 * pending-open handle -2 is 0x00000000FFFFFFFE as an `int fd` argument and
 * 0xFFFFFFFFFFFFFFFE as a `long fd` argument, so a w_dep=0 call taking
 * `int` and an r_dep=0 call taking `long` on that handle are not ordered.
 * Non-negative values are unaffected.
 *
 * A fix needs the signedness the pass does not record today: stage signed
 * arguments sign-extended, or record each dependency argument's width and
 * sign in the descriptor and normalize here. It is left alone for now because
 * it only bites when one resource is declared with two different integer
 * types; declare dependency arguments with one type until then. */
static bool same_dependency_key(const struct filc_async_task* a, size_t ai,
                                const struct filc_async_task* b, size_t bi)
{
    unsigned ad = a->meta->args[ai].dependency;
    unsigned bd = b->meta->args[bi].dependency;
    if ((ad & FILC_ASYNC_DEP_POINTER) != (bd & FILC_ASYNC_DEP_POINTER))
        return false;
    const filc_async_arg* aa = (const filc_async_arg*)a->staged_args;
    const filc_async_arg* ba = (const filc_async_arg*)b->staged_args;
    if (ad & FILC_ASYNC_DEP_POINTER) {
        void* ap = arg_ptr(aa, ai);
        void* bp = arg_ptr(ba, bi);
        return (ap ? zgetlower(ap) : NULL) == (bp ? zgetlower(bp) : NULL);
    }
    return arg_word(aa, ai) == arg_word(ba, bi);
}

static bool tasks_conflict(const struct filc_async_task* a,
                           const struct filc_async_task* b)
{
    for (size_t ai = 0; ai < a->meta->nargs; ++ai) {
        unsigned am = a->meta->args[ai].dependency & ~FILC_ASYNC_DEP_POINTER;
        if (!am)
            continue;
        for (size_t bi = 0; bi < b->meta->nargs; ++bi) {
            unsigned bm = b->meta->args[bi].dependency & ~FILC_ASYNC_DEP_POINTER;
            if (bm && (am == FILC_ASYNC_DEP_WRITE ||
                       bm == FILC_ASYNC_DEP_WRITE) &&
                same_dependency_key(a, ai, b, bi))
                return true;
        }
    }
    return false;
}

static struct filc_async_task* predecessor(const struct filc_async_task* t)
{
    /* The list is newest first; t->next contains exactly the earlier calls. */
    for (struct filc_async_task* p = t->next; p; p = p->next)
        if (p->state == 1 && tasks_conflict(p, t))
            return p;
    return NULL;
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
static void start_ready_tasks(void);

static void complete_task(struct filc_async_task* t, long result)
{
    if (t->state != 1) {
        return;
    }
    t->request = 0;
    t->result = result;
    t->state = result < 0 ? 2 : 0;
    ++g_completed;
    if (result < 0)
        ++g_failed;
    auto_resolve_buffers(t);
    /* Move t from g_tasks to g_done. t->next is left intact so a walker
     * already standing on t still reaches the rest of g_tasks. */
    for (struct filc_async_task** link = &g_tasks; *link; link = &(*link)->next) {
        if (*link == t) {
            *link = t->next;
            break;
        }
    }
    t->done_next = g_done;
    g_done = t;
    start_ready_tasks();
}

static void refresh_task(struct filc_async_task* t, bool wait)
{
    if (t->state != 1)
        return;
    if (!t->started) {
        for (struct filc_async_task* p = t->next; p; p = p->next)
            if (p->state == 1 && tasks_conflict(p, t))
                refresh_task(p, wait);
        start_ready_tasks();
        if (!t->started)
            return;
    }
    if (!t->request) {
        complete_task(t, t->result);
        return;
    }
    /* Submit only for a request that is still running. One the access hook
     * already resolved needs nothing, and submitting would flush whatever the
     * program queued since: one kernel entry per call that retires a stale
     * mark, instead of one per batch. */
    if (!fasync_ready(t->request)) {
        if (!wait)
            return;
        if (fasync_submit() < 0)
            filc_async_fatal("io_uring submission failed");
    }
    complete_task(t, fasync_result(t->request));
}

/* Returns false only when the request could not be queued because the
 * request table is full; the task is then left unstarted for its submit to
 * retry once reclaim_tasks() has freed a slot. */
static bool start_task(struct filc_async_task* t)
{
    bool ready = t->state == 1 && !t->started && !predecessor(t);
    if (!ready) {
        return true;
    }
    t->started = 1;

    const filc_async_arg* args = (const filc_async_arg*)t->staged_args;
    enum fasync_op op = opcode_from(t->meta->opts);
    long failure = 0;
    if (op == FASYNC_OP_IGNORE) {
        /* Keep the mark until poll/wait observes this test-only task. */
        t->result = -EOPNOTSUPP;
    } else if (!shape_ok(t->meta, op)) {
        failure = -EINVAL;
    } else if ((op == FASYNC_OP_READ || op == FASYNC_OP_WRITE) &&
               arg_word(args, 2) > UINT_MAX) {
        failure = -EOVERFLOW;
    } else {
        errno = 0;
        t->request = dispatch_request(op, args);
        if (!t->request)
            failure = errno ? -errno : -EIO;
    }
    if (failure == -EAGAIN) {
        t->started = 0;
        return false;
    }
    if (failure)
        complete_task(t, failure);
    return true;
}

static void start_ready_tasks(void)
{
    for (struct filc_async_task* t = g_tasks; t; t = t->next)
        start_task(t);
}

/* Complete in-flight tasks whose requests have finished without anyone
 * observing it, typically because the program resolved their buffers through
 * the compiler's access hook and never calls poll/wait. That returns their
 * request slots and pending marks. If none has finished, wait for the oldest
 * started one. Returns false when no started task is in flight at all. */
static bool reclaim_tasks(void)
{
    bool reclaimed = false;
    struct filc_async_task* oldest = NULL;
    for (struct filc_async_task* t = g_tasks; t; t = t->next) {
        if (t->state != 1 || !t->started)
            continue;
        refresh_task(t, false);
        if (t->state != 1)
            reclaimed = true;
        else
            oldest = t;
    }
    if (reclaimed)
        return true;
    if (!oldest)
        return false;
    refresh_task(oldest, true);
    return true;
}

void* filc_async_submit(const filc_async_meta* meta, void* impl, void* opts,
                        void* staged_args, size_t nargs)
{
    fasync_check_thread();
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
    t->started = 0;

    // The compiler marks output buffers just before calling submit. Attach
    // those marks to this request so re-marking can wait for the right owner.
    const filc_async_arg* args = (const filc_async_arg*)staged_args;
    t->next = g_tasks;
    g_tasks = t;
    ++g_submitted;
    for (size_t i = 0; i < nargs; ++i) {
        uint32_t kind = meta->args[i].kind;
        if (kind != FILC_ASYNC_ARG_BUFFER_OUT && kind != FILC_ASYNC_ARG_PENDING)
            continue;
        void* buf = arg_ptr(args, i);
        for (size_t j = 0; j < g_npending; ++j)
            if (g_pending[j].buf == buf && !g_pending[j].owner)
                g_pending[j].owner = t;
    }
    start_ready_tasks();

    /* A returned task must already have an io_uring request. The Fil-C
     * access hook can only resolve buffers represented in that request table;
     * waiting here preserves lazy buffer reads for dependent operations. */
    for (;;) {
        bool started = t->started;
        struct filc_async_task* prior = started ? NULL : predecessor(t);
        if (started)
            break;
        if (prior) {
            refresh_task(prior, true);
        } else if (!start_task(t) && !reclaim_tasks()) {
            /* The table is full of requests this runtime does not own. */
            t->started = 1;
            complete_task(t, -EAGAIN);
        }
    }

    return (void*)t;
}

// On completion (poll/wait), clear the pending marks the pass set for this
// task's producing buffers. Idempotent per task.
static void auto_resolve_buffers(struct filc_async_task* t)
{
    /* Called by complete_task. */
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
    fasync_check_thread();
    if (!out || !out->pending)
        return false;
    struct filc_async_task* t = find_task(out->pending);
    if (!t)
        return false;
    /* Look before submitting, like refresh_task; a task still running gets
     * the queue sent so a polling loop makes progress. */
    refresh_task(t, false);
    if (t->state == 1) {
        if (fasync_submit() < 0)
            filc_async_fatal("io_uring submission failed");
        refresh_task(t, false);
    }
    result_fill(out, t);
    if (t->state == 1)
        return false;
    retire_task(t);
    return true;
}

void filc_async_wait(struct filc_async_result_s* out)
{
    fasync_check_thread();
    if (!out || !out->pending)
        return;
    struct filc_async_task* t = find_task(out->pending);
    if (!t)
        return;
    refresh_task(t, true);
    result_fill(out, t);
    retire_task(t);
}

void filc_async_mark_pending(void* buf)
{
    fasync_check_thread();
    if (!buf)
        return;
    uintptr_t lower = (uintptr_t)zgetlower(buf);
    uintptr_t upper = (uintptr_t)zgetupper(buf);

    // Waiting for the owner of an overlapping mark retires that mark, so the
    // loop rescans until the buffer can be marked.
    for (;;) {
        size_t i = 0;
        for (; i < g_npending; ++i) {
            uintptr_t ilower = (uintptr_t)zgetlower(g_pending[i].buf);
            uintptr_t iupper = (uintptr_t)zgetupper(g_pending[i].buf);
            if (lower < iupper && ilower < upper)
                break;
        }
        if (i == g_npending) {
            if (g_npending >= FASYNC_PENDING_REGISTRY_CAPACITY) {
                if (!reclaim_tasks())
                    filc_async_fatal("too many pending buffers");
                continue;
            }
            g_pending[g_npending].buf = buf;
            g_pending[g_npending].owner = NULL;
            g_npending++;
            return;
        }
        filc_async_pending_mark mark = g_pending[i];

        if (mark.owner) {
            refresh_task(mark.owner, true);
        } else {
            // Standalone marks may describe an explicit fasync request.
            fasync_resolve_pending(mark.buf, 1);
            for (size_t j = 0; j < g_npending; ++j)
                if (g_pending[j].buf == mark.buf && !g_pending[j].owner) {
                    g_pending[j] = g_pending[--g_npending];
                    break;
                }
        }
        g_pending_resolves++;
    }
}

void filc_async_mark_resolved(void* buf)
{
    fasync_check_thread();
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
    fasync_check_thread();
    if (!buf)
        return false;
    // The compiler's access hook may have reaped a CQE without entering this
    // API. Observe those completions before reporting registry state.
    for (size_t i = 0; i < g_npending;) {
        struct filc_async_task* owner = g_pending[i].owner;
        if (owner && owner->request && owner->state == 1)
            refresh_task(owner, false);
        /* completing owner removed its marks; slot i may now hold another */
        if (i < g_npending && g_pending[i].owner == owner)
            ++i;
    }
    uintptr_t lower = (uintptr_t)zgetlower((void*)buf);
    uintptr_t upper = (uintptr_t)zgetupper((void*)buf);
    for (size_t i = 0; i < g_npending; ++i) {
        uintptr_t ilower = (uintptr_t)zgetlower(g_pending[i].buf);
        uintptr_t iupper = (uintptr_t)zgetupper(g_pending[i].buf);
        if (lower < iupper && ilower < upper) {
            return true;
        }
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
