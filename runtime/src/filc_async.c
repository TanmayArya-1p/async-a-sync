#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <pizlonated_syscalls.h>
#include <stdfil.h>

#include "fasync.h"
#include "fasync_shared.h"
#include "filc_async.h"
#include "filc_async_alloc.h"

// The annotated-call ABI, on the same io_uring requests as fasync_*. The
// annotated body is linked but never executed. The task list is also the GC
// root for every in-flight pointer argument.
// See wiki/2026-09-29-pragma-async-runtime-interface.md.

struct filc_async_task;
static struct filc_async_task* g_tasks;
// Assigns call order and retires dependency claims. Never held across a kernel
// wait, so independent reads stay in flight together.
static pthread_mutex_t g_dependency_mutex = PTHREAD_MUTEX_INITIALIZER;

// pending-buffer registry. is_pending tests range overlap, so an alias of a
// marked buffer probes true. A mark holds the real pointer, which keeps its
// capability; bounds come from zgetlower/zgetupper.
#define FASYNC_PENDING_REGISTRY_CAPACITY FASYNC_MAX_INFLIGHT

typedef struct {
    void* buf;
    struct filc_async_task* owner;
} filc_async_pending_mark;

static filc_async_pending_mark g_pending[FASYNC_PENDING_REGISTRY_CAPACITY];
static size_t g_npending;

// Bumped once per stale mark resolved before re-marking.
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

// Validates noped_args, the emitted dependency words, and each operation's
// expected argument shape. The runtime owns the op list; the pass does not
// validate op=.
static bool arg_kinds_ok(const filc_async_meta* m)
{
    if (!m)
        return false;
    if (m->noped_args > m->nargs)
        return false;
    for (size_t i = 0; i < m->nargs; ++i) {
        unsigned dep = m->args[i].dependency;
        unsigned mode = dep & (FILC_ASYNC_DEP_READ | FILC_ASYNC_DEP_WRITE);
        if (dep & ~(FILC_ASYNC_DEP_READ | FILC_ASYNC_DEP_WRITE |
                    FILC_ASYNC_DEP_POINTER |
                    (FILC_ASYNC_DEP_NAMESPACE_MASK
                     << FILC_ASYNC_DEP_NAMESPACE_SHIFT)))
            return false;
        if (mode == FILC_ASYNC_DEP_NONE && dep != FILC_ASYNC_DEP_NONE)
            return false;
        if (mode != FILC_ASYNC_DEP_NONE && mode != FILC_ASYNC_DEP_READ &&
            mode != FILC_ASYNC_DEP_WRITE)
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

static filc_async_validator_fn g_validator;

static struct filc_async_task* find_task(const void* pending)
{
    pthread_mutex_lock(&g_dependency_mutex);
    for (struct filc_async_task* t = g_tasks; t; t = t->next) {
        if ((const void*)t == pending) {
            pthread_mutex_unlock(&g_dependency_mutex);
            return t;
        }
    }
    pthread_mutex_unlock(&g_dependency_mutex);
    return NULL;
}

// nargs 16-byte cells, one argument in the first word of each. A pointer cell
// keeps its capability, which claim_range's checks need.
static uint64_t arg_word(const filc_async_arg* args, size_t index)
{
    return args[index].value.word;
}

static void* arg_ptr(const filc_async_arg* args, size_t index)
{
    return args[index].value.ptr;
}

// Equal-valued keys are different resources unless the namespace hash matches.
static bool same_dependency_key(const struct filc_async_task* a, size_t ai,
                                const struct filc_async_task* b, size_t bi)
{
    unsigned ad = a->meta->args[ai].dependency;
    unsigned bd = b->meta->args[bi].dependency;
    if ((ad & FILC_ASYNC_DEP_POINTER) != (bd & FILC_ASYNC_DEP_POINTER))
        return false;
    if ((ad >> FILC_ASYNC_DEP_NAMESPACE_SHIFT) !=
        (bd >> FILC_ASYNC_DEP_NAMESPACE_SHIFT))
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
        unsigned am = a->meta->args[ai].dependency &
                      (FILC_ASYNC_DEP_READ | FILC_ASYNC_DEP_WRITE);
        if (!am)
            continue;
        for (size_t bi = 0; bi < b->meta->nargs; ++bi) {
            unsigned bm = b->meta->args[bi].dependency &
                          (FILC_ASYNC_DEP_READ | FILC_ASYNC_DEP_WRITE);
            if (bm && (am == FILC_ASYNC_DEP_WRITE ||
                       bm == FILC_ASYNC_DEP_WRITE) &&
                same_dependency_key(a, ai, b, bi))
                return true;
        }
    }
    return false;
}

static struct filc_async_task* predecessor_locked(const struct filc_async_task* t)
{
    // The list is newest first, so t->next is exactly the earlier calls.
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
    pthread_mutex_lock(&g_dependency_mutex);
    if (t->state != 1) {
        pthread_mutex_unlock(&g_dependency_mutex);
        return;
    }
    t->request = 0;
    t->result = result;
    t->state = result < 0 ? 2 : 0;
    ++g_completed;
    if (result < 0)
        ++g_failed;
    auto_resolve_buffers(t);
    pthread_mutex_unlock(&g_dependency_mutex);
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
    if (wait && fasync_submit() < 0)
        filc_async_fatal("io_uring submission failed");
    if (!wait && !fasync_ready(t->request))
        return;
    complete_task(t, fasync_result(t->request));
}

static void start_task(struct filc_async_task* t)
{
    pthread_mutex_lock(&g_dependency_mutex);
    bool ready = t->state == 1 && !t->started && !predecessor_locked(t);
    if (!ready) {
        pthread_mutex_unlock(&g_dependency_mutex);
        return;
    }
    t->started = 1;

    const filc_async_arg* args = (const filc_async_arg*)t->staged_args;
    enum fasync_op op = opcode_from(t->meta->opts);
    long failure = 0;
    if (op == FASYNC_OP_IGNORE) {
        // Keep the mark until poll/wait observes this test-only task.
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
    pthread_mutex_unlock(&g_dependency_mutex);
    if (failure)
        complete_task(t, failure);
}

static void start_ready_tasks(void)
{
    pthread_mutex_lock(&g_dependency_mutex);
    struct filc_async_task* head = g_tasks;
    pthread_mutex_unlock(&g_dependency_mutex);
    for (struct filc_async_task* t = head; t; t = t->next)
        start_task(t);
}

// Claims buf for `owner`, resolving any older overlapping claim first and
// holding it to completion. owner == NULL is a standalone public mark.
// The mutex is released before the wait so the completion callback can take it
// to retire the old claim before this loop retries.
static void claim_range(void* buf, struct filc_async_task* owner)
{
    if (!buf)
        return;
    uintptr_t lower = (uintptr_t)zgetlower(buf);
    uintptr_t upper = (uintptr_t)zgetupper(buf);
    for (;;) {
        pthread_mutex_lock(&g_dependency_mutex);
        size_t i = 0;
        for (; i < g_npending; ++i) {
            uintptr_t ilower = (uintptr_t)zgetlower(g_pending[i].buf);
            uintptr_t iupper = (uintptr_t)zgetupper(g_pending[i].buf);
            if (lower < iupper && ilower < upper)
                break;
        }
        if (i == g_npending) {
            if (g_npending >= FASYNC_PENDING_REGISTRY_CAPACITY)
                filc_async_fatal("too many pending buffers");
            g_pending[g_npending].buf = buf;
            g_pending[g_npending].owner = owner;
            g_npending++;
            pthread_mutex_unlock(&g_dependency_mutex);
            return;
        }
        filc_async_pending_mark mark = g_pending[i];
        pthread_mutex_unlock(&g_dependency_mutex);

        if (mark.owner) {
            refresh_task(mark.owner, true);
        } else {
            // A standalone mark may describe an explicit fasync request.
            fasync_resolve_pending(mark.buf, 1);
            pthread_mutex_lock(&g_dependency_mutex);
            for (size_t j = 0; j < g_npending; ++j)
                if (g_pending[j].buf == mark.buf && !g_pending[j].owner) {
                    g_pending[j] = g_pending[--g_npending];
                    break;
                }
            pthread_mutex_unlock(&g_dependency_mutex);
        }
        pthread_mutex_lock(&g_dependency_mutex);
        g_pending_resolves++;
        pthread_mutex_unlock(&g_dependency_mutex);
    }
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
    t->started = 0;
    t->request = 0;
    t->result = 0;
    t->next = NULL;

    // Mark this call's producing args (bout=, bare buf=, unannotated pointers)
    // and claim them, so call sites need no mark_pending. bin= is never marked.
    // Before dispatch, so a request completing inside submit retires marks that
    // already exist.
    const filc_async_arg* args = (const filc_async_arg*)staged_args;
    for (size_t i = 0; i < nargs; ++i) {
        uint32_t kind = meta->args[i].kind;
        if (!is_output_kind(kind))
            continue;
        claim_range(arg_ptr(args, i), t);
    }

    // Register the task (newest first) so predecessor_locked and the access
    // hook can find it, then dispatch whatever has no conflicting predecessor.
    pthread_mutex_lock(&g_dependency_mutex);
    t->next = g_tasks;
    g_tasks = t;
    ++g_submitted;
    pthread_mutex_unlock(&g_dependency_mutex);
    start_ready_tasks();

    // A returned task must already have a request: the access hook can only
    // resolve buffers in that request table, so this preserves lazy reads.
    for (;;) {
        pthread_mutex_lock(&g_dependency_mutex);
        bool started = t->started;
        struct filc_async_task* prior = started ? NULL : predecessor_locked(t);
        pthread_mutex_unlock(&g_dependency_mutex);
        if (started)
            break;
        if (prior)
            refresh_task(prior, true);
        else
            start_task(t);
    }

    return (void*)t;
}

// Clears the pending marks this task owns. Idempotent; called under the
// dependency mutex.
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
    if (t->state == 1 && fasync_submit() < 0)
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
    claim_range(buf, NULL);
}

void filc_async_mark_resolved(void* buf)
{
    if (!buf)
        return;
    uintptr_t lower = (uintptr_t)zgetlower(buf);
    pthread_mutex_lock(&g_dependency_mutex);
    for (size_t i = 0; i < g_npending; ++i)
        if ((uintptr_t)zgetlower(g_pending[i].buf) == lower) {
            g_pending[i] = g_pending[g_npending - 1];
            g_npending--;
            pthread_mutex_unlock(&g_dependency_mutex);
            return;
        }
    pthread_mutex_unlock(&g_dependency_mutex);
}

bool filc_async_is_pending(const void* buf)
{
    if (!buf)
        return false;
    // The compiler's access hook may have reaped a CQE without entering this
    // API. Observe those completions before reporting registry state.
    for (size_t i = 0;;) {
        pthread_mutex_lock(&g_dependency_mutex);
        if (i >= g_npending) {
            pthread_mutex_unlock(&g_dependency_mutex);
            break;
        }
        struct filc_async_task* owner = g_pending[i].owner;
        pthread_mutex_unlock(&g_dependency_mutex);
        if (owner && owner->request && owner->state == 1)
            refresh_task(owner, false);
        pthread_mutex_lock(&g_dependency_mutex);
        if (i < g_npending && g_pending[i].owner == owner)
            ++i;
        pthread_mutex_unlock(&g_dependency_mutex);
    }
    uintptr_t lower = (uintptr_t)zgetlower((void*)buf);
    uintptr_t upper = (uintptr_t)zgetupper((void*)buf);
    pthread_mutex_lock(&g_dependency_mutex);
    for (size_t i = 0; i < g_npending; ++i) {
        uintptr_t ilower = (uintptr_t)zgetlower(g_pending[i].buf);
        uintptr_t iupper = (uintptr_t)zgetupper(g_pending[i].buf);
        if (lower < iupper && ilower < upper) {
            pthread_mutex_unlock(&g_dependency_mutex);
            return true;
        }
    }
    pthread_mutex_unlock(&g_dependency_mutex);
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
    pthread_mutex_lock(&g_dependency_mutex);
    struct filc_async_task* head = g_tasks;
    pthread_mutex_unlock(&g_dependency_mutex);
    for (struct filc_async_task* t = head; t; t = t->next)
        if (t->request && t->state == 1)
            refresh_task(t, false);
    *out = (filc_async_stats){ 0 };
    pthread_mutex_lock(&g_dependency_mutex);
    out->tasks_submitted = g_submitted;
    out->tasks_completed = g_completed;
    out->tasks_failed = g_failed;
    out->pending_resolves = g_pending_resolves;
    pthread_mutex_unlock(&g_dependency_mutex);
    struct fasync_stats stats;
    fasync_get_stats(&stats);
    out->sqes_queued = stats.sqes_queued;
    out->kernel_submit_entries = stats.kernel_submit_entries;
    out->kernel_wait_entries = stats.kernel_wait_entries;
}

void filc_async_set_validator(filc_async_validator_fn fn)
{
    g_validator = fn;
}

void filc_async_validate_table(const filc_async_meta* const* metas)
{
    filc_async_validator_fn validator = g_validator ? g_validator : default_validator;
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