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
 * Every per-call operation here costs the same however many calls are in
 * flight, because a program can have a thousand in flight at once:
 *
 *   g_tasks        the tasks in flight, newest first, doubly linked; only
 *                  reclaiming, statistics and rare waits walk it
 *   g_handles      the tasks whose completion poll/wait has not delivered,
 *                  hashed by address, so a handle is found without a walk
 *   g_mark_buckets pending marks, hashed by the marked object
 *   g_dep_buckets  in-flight tasks by dependency key, for ordering
 *
 * A program that only ever touches its buffers never delivers, so g_handles
 * can grow; nothing but a poll/wait looking up its handle reads it. */

struct filc_async_task;
struct filc_async_mark;
struct filc_async_dep_ref;

static struct filc_async_task* g_tasks;
static unsigned long g_seq;      /* submission order */
static size_t g_unstarted;       /* tasks in g_tasks not started yet */

static unsigned long g_submitted;
static unsigned long g_completed;
static unsigned long g_failed;

// mark_pending bumped once per stale mark it resolved before re-marking.
static unsigned long g_pending_resolves;

struct filc_async_task {
    struct filc_async_task* next;        /* g_tasks: older */
    struct filc_async_task* prev;        /* g_tasks: newer */
    struct filc_async_task* handle_next; /* g_handles bucket */
    void* impl;
    void* opts;
    const filc_async_meta* meta;
    void* staged_args;      /* keeps staged pointer capabilities alive */
    unsigned long seq;      /* submission order */
    struct filc_async_mark* marks;     /* pending marks this task owns */
    struct filc_async_dep_ref* deps;   /* its dependency keys, ndeps long */
    unsigned ndeps;
    unsigned char state;    /* 0 done, 1 pending, 2 failed */
    unsigned char started;  /* an SQE has been issued (or an error recorded) */
    unsigned char resolved; /* auto-resolve already ran for this task */
    unsigned char handled;  /* in g_handles */
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

/* Multiplicative hashing: the top `bits` bits of key * 2^64/phi. */
static size_t hash_bits(uintptr_t key, unsigned bits)
{
    return (size_t)(((uint64_t)key * 0x9E3779B97F4A7C15ULL) >> (64 - bits));
}

/* ---- Handles not yet delivered ---- */

static struct filc_async_task** g_handles;
static unsigned g_handle_bits;
static size_t g_nhandles;

/* Doubles the table. The old one stays with the allocator, like tasks. */
static void handles_grow(void)
{
    unsigned bits = g_handles ? g_handle_bits + 1 : 10;
    size_t n = (size_t)1 << bits;
    struct filc_async_task** table =
        (struct filc_async_task**)filc_async_alloc(n * sizeof *table, 16);
    if (!table)
        filc_async_fatal("filc_async_submit: out of memory");
    for (size_t i = 0; g_handles && i < ((size_t)1 << g_handle_bits); ++i) {
        struct filc_async_task* t = g_handles[i];
        while (t) {
            struct filc_async_task* next = t->handle_next;
            size_t b = hash_bits((uintptr_t)t, bits);
            t->handle_next = table[b];
            table[b] = t;
            t = next;
        }
    }
    g_handles = table;
    g_handle_bits = bits;
}

static void handle_add(struct filc_async_task* t)
{
    if (!g_handles || g_nhandles >= ((size_t)1 << g_handle_bits))
        handles_grow();
    size_t b = hash_bits((uintptr_t)t, g_handle_bits);
    t->handle_next = g_handles[b];
    g_handles[b] = t;
    t->handled = 1;
    ++g_nhandles;
}

static struct filc_async_task* find_task(const void* pending)
{
    if (!g_handles)
        return NULL;
    size_t b = hash_bits((uintptr_t)pending, g_handle_bits);
    for (struct filc_async_task* t = g_handles[b]; t; t = t->handle_next)
        if ((const void*)t == pending)
            return t;
    return NULL;
}

/* A task whose completion has been delivered leaves g_handles; later
 * poll/wait calls on its handle find nothing and leave the caller's result
 * alone. A task still in flight keeps its handle. */
static void retire_task(struct filc_async_task* t)
{
    if (t->state == 1 || !t->handled)
        return;
    struct filc_async_task** link =
        &g_handles[hash_bits((uintptr_t)t, g_handle_bits)];
    while (*link != t)
        link = &(*link)->handle_next;
    *link = t->handle_next;
    t->handled = 0;
    --g_nhandles;
}

/* ---- The in-flight list ---- */

static void tasks_push(struct filc_async_task* t)
{
    t->prev = NULL;
    t->next = g_tasks;
    if (g_tasks)
        g_tasks->prev = t;
    g_tasks = t;
}

/* t->next is left intact so a walker already standing on t still reaches the
 * rest of g_tasks. */
static void tasks_unlink(struct filc_async_task* t)
{
    if (t->prev)
        t->prev->next = t->next;
    else
        g_tasks = t->next;
    if (t->next)
        t->next->prev = t->prev;
}

static void set_started(struct filc_async_task* t, unsigned char started)
{
    if (t->started == started)
        return;
    t->started = started;
    if (started)
        --g_unstarted;
    else
        ++g_unstarted;
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

/* ---- Dependency index ----
 *
 * Submission order assigns the call order, and completion retires a task's
 * claims. Each in-flight task has one ref per dependency argument, filed
 * under that argument's key in a readers or a writers list, newest first.
 * Two calls conflict when they share a key and one of them writes it, so a
 * call's newest conflicting predecessor is at or near the head of its keys'
 * lists, however many unrelated calls are in flight. The runtime is
 * single-threaded (see fasync_check_thread), so none of this is locked. */

struct filc_async_dep_key {
    struct filc_async_dep_key* bucket_next; /* same bucket, or free list */
    uint64_t word;
    unsigned char pointer;
    struct filc_async_dep_ref* readers;     /* in flight, newest first */
    struct filc_async_dep_ref* writers;
};

struct filc_async_dep_ref {
    struct filc_async_task* task;
    struct filc_async_dep_key* key;
    struct filc_async_dep_ref* newer;
    struct filc_async_dep_ref* older;
    unsigned char write;
};

#define DEP_BUCKET_BITS 10
static struct filc_async_dep_key* g_dep_buckets[1u << DEP_BUCKET_BITS];
static struct filc_async_dep_key* g_free_keys;

/* The key of dependency argument i. Scalar keys compare by value; pointer
 * keys compare by object identity, so they key on the object's lower bound.
 *
 * FIXME: scalar keys depend on the declared integer width.
 *
 * FilAsync stages an integer narrower than 64 bits zero-extended
 * (Builder.CreateZExt in FilAsync.cpp's call rewriting), and scalar keys are
 * the full 64-bit words. The same value declared with different widths
 * therefore does not always match. For example, a pending-open handle -2 is
 * 0x00000000FFFFFFFE as an `int fd` argument and 0xFFFFFFFFFFFFFFFE as a
 * `long fd` argument, so a w_dep=0 call taking `int` and an r_dep=0 call
 * taking `long` on that handle are not ordered. Non-negative values are
 * unaffected.
 *
 * A fix needs the signedness the pass does not record today: stage signed
 * arguments sign-extended, or record each dependency argument's width and
 * sign in the descriptor and normalize here. It is left alone for now because
 * it only bites when one resource is declared with two different integer
 * types; declare dependency arguments with one type until then. */
static uint64_t dep_key_word(const filc_async_arg* args, size_t i, unsigned dep)
{
    if (dep & FILC_ASYNC_DEP_POINTER) {
        void* p = arg_ptr(args, i);
        return (uint64_t)(uintptr_t)(p ? zgetlower(p) : NULL);
    }
    return arg_word(args, i);
}

static struct filc_async_dep_key* dep_key_get(uint64_t word, unsigned char pointer)
{
    struct filc_async_dep_key** bucket =
        &g_dep_buckets[hash_bits((uintptr_t)(word ^ pointer), DEP_BUCKET_BITS)];
    for (struct filc_async_dep_key* k = *bucket; k; k = k->bucket_next)
        if (k->word == word && k->pointer == pointer)
            return k;
    struct filc_async_dep_key* k = g_free_keys;
    if (k)
        g_free_keys = k->bucket_next;
    else
        k = (struct filc_async_dep_key*)filc_async_alloc(sizeof *k, 16);
    if (!k)
        filc_async_fatal("filc_async_submit: out of memory");
    k->word = word;
    k->pointer = pointer;
    k->readers = NULL;
    k->writers = NULL;
    k->bucket_next = *bucket;
    *bucket = k;
    return k;
}

/* Files t's dependency arguments under their keys. */
static void deps_index(struct filc_async_task* t)
{
    const filc_async_meta* m = t->meta;
    const filc_async_arg* args = (const filc_async_arg*)t->staged_args;
    unsigned n = 0;
    for (size_t i = 0; i < m->nargs; ++i)
        n += (m->args[i].dependency & ~FILC_ASYNC_DEP_POINTER) != 0;
    if (!n)
        return;
    t->deps = (struct filc_async_dep_ref*)filc_async_alloc(n * sizeof *t->deps, 16);
    if (!t->deps)
        filc_async_fatal("filc_async_submit: out of memory");
    for (size_t i = 0; i < m->nargs; ++i) {
        unsigned dep = m->args[i].dependency;
        unsigned mode = dep & ~FILC_ASYNC_DEP_POINTER;
        if (!mode)
            continue;
        struct filc_async_dep_ref* r = &t->deps[t->ndeps++];
        r->task = t;
        r->write = mode == FILC_ASYNC_DEP_WRITE;
        r->key = dep_key_get(dep_key_word(args, i, dep),
                             (dep & FILC_ASYNC_DEP_POINTER) != 0);
        struct filc_async_dep_ref** head =
            r->write ? &r->key->writers : &r->key->readers;
        r->newer = NULL;
        r->older = *head;
        if (*head)
            (*head)->newer = r;
        *head = r;
    }
}

/* Takes t's refs out of the index; a key nobody uses goes back to the free
 * list. */
static void deps_unindex(struct filc_async_task* t)
{
    for (unsigned i = 0; i < t->ndeps; ++i) {
        struct filc_async_dep_ref* r = &t->deps[i];
        struct filc_async_dep_key* k = r->key;
        if (r->newer)
            r->newer->older = r->older;
        else if (r->write)
            k->writers = r->older;
        else
            k->readers = r->older;
        if (r->older)
            r->older->newer = r->newer;
        if (k->readers || k->writers)
            continue;
        struct filc_async_dep_key** link =
            &g_dep_buckets[hash_bits((uintptr_t)(k->word ^ k->pointer),
                                     DEP_BUCKET_BITS)];
        while (*link != k)
            link = &(*link)->bucket_next;
        *link = k->bucket_next;
        k->bucket_next = g_free_keys;
        g_free_keys = k;
    }
    t->ndeps = 0;
}

/* The newest task in `list` submitted before `before`. Every task in the
 * index is in flight. */
static struct filc_async_task* newest_before(const struct filc_async_dep_ref* list,
                                             unsigned long before)
{
    for (; list; list = list->older)
        if (list->task->seq < before)
            return list->task;
    return NULL;
}

/* The newest in-flight task submitted before `before` that conflicts with t:
 * one that writes a key t uses, or uses a key t writes. */
static struct filc_async_task* conflict_before(const struct filc_async_task* t,
                                               unsigned long before)
{
    struct filc_async_task* best = NULL;
    for (unsigned i = 0; i < t->ndeps; ++i) {
        const struct filc_async_dep_ref* r = &t->deps[i];
        struct filc_async_task* p = newest_before(r->key->writers, before);
        if (p && (!best || p->seq > best->seq))
            best = p;
        if (!r->write)
            continue;
        p = newest_before(r->key->readers, before);
        if (p && (!best || p->seq > best->seq))
            best = p;
    }
    return best;
}

static struct filc_async_task* predecessor(const struct filc_async_task* t)
{
    return conflict_before(t, t->seq);
}

/* ---- Pending-buffer registry ----
 *
 * mark_pending/mark_resolved add and remove marks; is_pending tests range
 * overlap so aliases of a marked buffer probe true. A mark covers the whole
 * object, [zgetlower, zgetupper), and holds the real buffer pointer so a
 * resolve keeps its capability. Live objects never overlap, so a mark that
 * overlaps an object's range has that object's lower bound: marks are hashed
 * by it, and every lookup reads one bucket. Re-marking resolves the prior
 * mark first. */
#define FASYNC_PENDING_REGISTRY_CAPACITY FASYNC_MAX_INFLIGHT
#define MARK_BUCKET_BITS 11

typedef struct filc_async_mark {
    void* buf;
    uintptr_t lower;
    uintptr_t upper;
    struct filc_async_task* owner;
    struct filc_async_mark* bucket_next; /* same bucket */
    struct filc_async_mark* owner_next;  /* the owner's marks, or free list */
} filc_async_mark;

static filc_async_mark g_marks[FASYNC_PENDING_REGISTRY_CAPACITY];
static size_t g_marks_carved; /* g_marks entries ever handed out */
static filc_async_mark* g_free_marks;
static filc_async_mark* g_mark_buckets[1u << MARK_BUCKET_BITS];
static size_t g_npending;

static filc_async_mark** mark_bucket(uintptr_t lower)
{
    return &g_mark_buckets[hash_bits(lower, MARK_BUCKET_BITS)];
}

static filc_async_mark* mark_find_overlap(uintptr_t lower, uintptr_t upper)
{
    for (filc_async_mark* m = *mark_bucket(lower); m; m = m->bucket_next)
        if (lower < m->upper && m->lower < upper)
            return m;
    return NULL;
}

/* The caller has checked g_npending against the capacity. */
static void mark_add(void* buf, uintptr_t lower, uintptr_t upper)
{
    filc_async_mark* m = g_free_marks;
    if (m)
        g_free_marks = m->owner_next;
    else
        m = &g_marks[g_marks_carved++];
    m->buf = buf;
    m->lower = lower;
    m->upper = upper;
    m->owner = NULL;
    m->owner_next = NULL;
    filc_async_mark** bucket = mark_bucket(lower);
    m->bucket_next = *bucket;
    *bucket = m;
    ++g_npending;
}

static void mark_remove(filc_async_mark* m)
{
    filc_async_mark** link = mark_bucket(m->lower);
    while (*link != m)
        link = &(*link)->bucket_next;
    *link = m->bucket_next;
    if (m->owner) {
        filc_async_mark** own = &m->owner->marks;
        while (*own != m)
            own = &(*own)->owner_next;
        *own = m->owner_next;
    }
    m->buf = NULL;
    m->owner = NULL;
    m->owner_next = g_free_marks;
    g_free_marks = m;
    --g_npending;
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
    deps_unindex(t);
    tasks_unlink(t);
    start_ready_tasks();
}

static void refresh_task(struct filc_async_task* t, bool wait)
{
    if (t->state != 1)
        return;
    if (!t->started) {
        /* Each conflicting predecessor once, newest first. */
        unsigned long before = t->seq;
        struct filc_async_task* p;
        while ((p = conflict_before(t, before))) {
            before = p->seq;
            refresh_task(p, wait);
        }
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
    set_started(t, 1);

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
        set_started(t, 0);
        return false;
    }
    if (failure)
        complete_task(t, failure);
    return true;
}

/* Tasks start inside their own submit unless a predecessor holds them, so
 * the unstarted ones are the newest few; stop once all have been tried. */
static void start_ready_tasks(void)
{
    size_t left = g_unstarted;
    for (struct filc_async_task* t = g_tasks; t && left; t = t->next) {
        if (t->state != 1 || t->started)
            continue;
        --left;
        start_task(t);
    }
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

    /* The allocator returns zeroed memory. */
    t->impl = impl;
    t->opts = opts;
    t->meta = meta;
    t->staged_args = staged_args;
    t->seq = ++g_seq;
    t->state = 1;
    ++g_unstarted;
    tasks_push(t);
    handle_add(t);
    deps_index(t);
    ++g_submitted;

    // The compiler marks output buffers just before calling submit. Attach
    // those marks to this request so re-marking can wait for the right owner.
    const filc_async_arg* args = (const filc_async_arg*)staged_args;
    for (size_t i = 0; i < nargs; ++i) {
        uint32_t kind = meta->args[i].kind;
        if (kind != FILC_ASYNC_ARG_BUFFER_OUT && kind != FILC_ASYNC_ARG_PENDING)
            continue;
        void* buf = arg_ptr(args, i);
        if (!buf)
            continue;
        for (filc_async_mark* m = *mark_bucket((uintptr_t)zgetlower(buf)); m;
             m = m->bucket_next)
            if (m->buf == buf && !m->owner) {
                m->owner = t;
                m->owner_next = t->marks;
                t->marks = m;
            }
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
            set_started(t, 1);
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
    while (t->marks)
        mark_remove(t->marks);
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
    // loop looks again until the buffer can be marked.
    for (;;) {
        filc_async_mark* m = mark_find_overlap(lower, upper);
        if (!m) {
            if (g_npending >= FASYNC_PENDING_REGISTRY_CAPACITY) {
                if (!reclaim_tasks())
                    filc_async_fatal("too many pending buffers");
                continue;
            }
            mark_add(buf, lower, upper);
            return;
        }
        if (m->owner) {
            refresh_task(m->owner, true);
        } else {
            // Standalone marks may describe an explicit fasync request.
            fasync_resolve_pending(m->buf, 1);
            mark_remove(m);
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
    for (filc_async_mark* m = *mark_bucket(lower); m; m = m->bucket_next)
        if (m->lower == lower) {
            mark_remove(m);
            return;
        }
}

bool filc_async_is_pending(const void* buf)
{
    fasync_check_thread();
    if (!buf)
        return false;
    uintptr_t lower = (uintptr_t)zgetlower((void*)buf);
    uintptr_t upper = (uintptr_t)zgetupper((void*)buf);
    filc_async_mark* m = mark_find_overlap(lower, upper);
    // The compiler's access hook may have reaped the owner's CQE without
    // entering this API. Observe that before reporting registry state.
    if (m && m->owner && m->owner->request && m->owner->state == 1) {
        refresh_task(m->owner, false);
        m = mark_find_overlap(lower, upper);
    }
    return m != NULL;
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
