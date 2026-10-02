#include <errno.h>
#include <pizlonated_syscalls.h>
#include <pthread.h>
#include <stdfil.h>
#include <string.h>
#include <time.h>

#include "filc_async.h"
#include "filc_async_alloc.h"
#include "filc_async_runtime.h"

/* The framework's native half (filc_async_native.c): the pending flag in an
 * object's header, which the compiler's access hook tests inline, and the
 * resolver that hook calls when it finds the flag set. `pending` is
 * PENDING_CLEAR, PENDING_SET, or PENDING_READERS: set, unless the object is
 * read-only, which the program cannot store into. */
enum { PENDING_CLEAR = 0, PENDING_SET = 1, PENDING_READERS = 2 };
void zasync_set_pending(void* buf, int pending);
void zasync_set_resolver(void (*resolver)(void*));

/* The generic framework behind annotated calls. The pass-emitted stub of each
 * annotated function calls filc_async_begin, takes the call's dependency
 * locks, marks its output buffers, and hands the call to its runtime with
 * filc_async_submit. This file knows nothing about any op or runtime: it
 * reaches each task's runtime only through the filc_async_runtime descriptor
 * its function names (filc_async_runtime.h), so one program may use several.
 *
 *   g_running      tasks the runtime has not completed, oldest first
 *   g_handles      tasks whose completion poll/wait has not delivered,
 *                  hashed by address, so a handle is found without a walk
 *   g_mark_buckets pending marks, hashed by the marked object
 *   g_lock_buckets dependency locks, hashed by value and namespace
 *
 * A program that only ever touches its buffers never delivers, so g_handles
 * can grow; nothing but a poll/wait looking up its handle reads it.
 *
 * Tasks, marks and locks live in the allocator arena.
 *
 * Threads: all of this state is guarded by g_lock, and g_changed is
 * signalled whenever a task completes or a dependency lock changes hands.
 * The framework never holds g_lock while it calls into the runtime, so a
 * runtime may report a completion from any thread while holding locks of its
 * own. */

struct filc_async_task;
struct filc_async_mark;
struct filc_async_hold;

struct filc_async_task {
    const filc_async_meta* meta;
    const filc_async_runtime* runtime; /* polled for this task's completion */
    void* staged_args;      /* keeps staged pointer capabilities alive */
    void* runtime_data;
    struct filc_async_task* newer;       /* g_running */
    struct filc_async_task* older;
    struct filc_async_task* handle_next; /* g_handles bucket */
    struct filc_async_mark* marks;       /* pending marks this task owns */
    struct filc_async_hold* holds;       /* dependency locks this task holds */
    unsigned char state;    /* 0 done, 1 running, 2 failed */
    unsigned char handled;  /* in g_handles */
    long result;
};

static struct filc_async_task* g_newest;
static struct filc_async_task* g_oldest;

static unsigned long g_submitted;
static unsigned long g_completed;
static unsigned long g_failed;
static unsigned long g_pending_resolves;
static unsigned long g_lock_waits;
static unsigned long g_hook_resolves;

/* The task whose body filc_async_run is running on this thread. */
static _Thread_local struct filc_async_task* g_running_body;
/* The task whose runtime submit is running on this thread. */
static _Thread_local struct filc_async_task* g_submitting;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_changed = PTHREAD_COND_INITIALIZER;

static void lock(void)
{
    pthread_mutex_lock(&g_lock);
}

static void unlock(void)
{
    pthread_mutex_unlock(&g_lock);
}

/* Waits, with g_lock held, for another thread to change something: a task
 * that is not yet submitted cannot be polled, only waited out. Bounded, so a
 * change signalled just before the wait is not missed for long. */
static void wait_changed(void)
{
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_nsec += 1000000;
    if (deadline.tv_nsec >= 1000000000) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000;
    }
    pthread_cond_timedwait(&g_changed, &g_lock, &deadline);
}

static filc_async_validator_fn g_validator;

static void* alloc_or_die(size_t size)
{
    void* p = filc_async_alloc(size, 16);
    if (!p)
        filc_async_fatal("out of memory");
    return p;
}

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
        (struct filc_async_task**)alloc_or_die(n * sizeof *table);
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
 * alone. A task still running keeps its handle. */
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

/* ---- Running tasks ---- */

static void running_add(struct filc_async_task* t)
{
    t->newer = NULL;
    t->older = g_newest;
    if (g_newest)
        g_newest->newer = t;
    else
        g_oldest = t;
    g_newest = t;
}

/* t->newer and t->older are left intact so a walker already standing on t
 * still reaches the rest of the list. */
static void running_remove(struct filc_async_task* t)
{
    if (t->newer)
        t->newer->older = t->older;
    else
        g_newest = t->older;
    if (t->older)
        t->older->newer = t->newer;
    else
        g_oldest = t->newer;
}

/* Blocks, with g_lock held, until the runtime has completed `t`. The lock is
 * released while the runtime is asked, so it can report completions. */
static void wait_for(struct filc_async_task* t)
{
    while (t->state == 1) {
        unlock();
        bool done = t->runtime->poll(t, FILC_ASYNC_POLL_BLOCK);
        lock();
        if (!done && t->state == 1)
            wait_changed();
    }
}

/* ---- Dependency locks ----
 *
 * One lock per (value, space), where space is the argument's pointer bit and
 * namespace. A call holding a read lock shares it with other readers; a
 * write lock is exclusive. A call that needs a lock another call holds in a
 * conflicting mode waits in its stub, by polling that call through the
 * runtime, until it completes and releases the lock. Requests queue on the
 * lock and are granted in the order they arrived, so a stream of readers
 * cannot keep a waiting writer out.
 *
 * Many calls can share one lock, such as reads of one file, so taking and
 * releasing it must not walk its holders: a lock counts its holders and its
 * writers, which says at once whether a request conflicts, and each holder
 * links back to its place in the list, so it leaves in constant time. */

struct filc_async_lock {
    struct filc_async_lock* bucket_next; /* same bucket, or free list */
    uint64_t value;
    uint32_t space;
    struct filc_async_hold* holders;
    struct filc_async_hold* waiters; /* requests not yet granted, oldest first */
    unsigned nholders;
    unsigned nwriters; /* holders in write mode */
};

struct filc_async_hold {
    struct filc_async_task* task;
    struct filc_async_lock* lock;
    struct filc_async_hold* task_next;  /* the task's holds */
    struct filc_async_hold* lock_next;  /* the lock's waiters, then holders */
    struct filc_async_hold** lock_pprev; /* what points at it, once a holder */
    unsigned char write;
};

#define LOCK_BUCKET_BITS 10
static struct filc_async_lock* g_lock_buckets[1u << LOCK_BUCKET_BITS];
static struct filc_async_lock* g_free_locks;

static struct filc_async_lock** lock_bucket(uint64_t value, uint32_t space)
{
    return &g_lock_buckets[hash_bits((uintptr_t)(value ^ space), LOCK_BUCKET_BITS)];
}

static struct filc_async_lock* lock_get(uint64_t value, uint32_t space)
{
    struct filc_async_lock** bucket = lock_bucket(value, space);
    for (struct filc_async_lock* l = *bucket; l; l = l->bucket_next)
        if (l->value == value && l->space == space)
            return l;
    struct filc_async_lock* l = g_free_locks;
    if (l)
        g_free_locks = l->bucket_next;
    else
        l = (struct filc_async_lock*)alloc_or_die(sizeof *l);
    l->value = value;
    l->space = space;
    l->holders = NULL;
    l->waiters = NULL;
    l->nholders = 0;
    l->nwriters = 0;
    l->bucket_next = *bucket;
    *bucket = l;
    return l;
}

/* A holder of `l` that `t` must wait for before taking it in `write` mode.
 * The counts rule out a conflict without a walk; the walk finds the holder
 * to wait for, and skips t's own holds. */
static struct filc_async_task* lock_conflict(const struct filc_async_lock* l,
                                             const struct filc_async_task* t,
                                             bool write)
{
    if (write ? l->nholders == 0 : l->nwriters == 0)
        return NULL;
    for (struct filc_async_hold* h = l->holders; h; h = h->lock_next)
        if (h->task != t && (write || h->write))
            return h->task;
    return NULL;
}

/* FIXME: scalar keys depend on the declared integer width.
 *
 * FilAsync passes an integer narrower than 64 bits zero-extended, so the same
 * value declared with different widths does not always match. For example, a
 * pending-open handle -2 is 0x00000000FFFFFFFE as an `int fd` argument and
 * 0xFFFFFFFFFFFFFFFE as a `long fd` argument, so a w_dep=fd:file call taking
 * `int` and an r_dep=fd:file call taking `long` on that handle are not
 * ordered.
 * Non-negative values are unaffected.
 *
 * A fix needs the signedness the pass does not record today: pass signed
 * arguments sign-extended, or record each dependency argument's width and
 * sign in the descriptor and normalize here. It is left alone for now because
 * it only bites when one resource is declared with two different integer
 * types; declare dependency arguments with one type until then. */
static void lock_take(struct filc_async_task* t, uint64_t value,
                      uint32_t space, uint32_t mode)
{
    bool write = mode == FILC_ASYNC_DEP_WRITE;
    struct filc_async_lock* l = lock_get(value, space);
    struct filc_async_hold* h = (struct filc_async_hold*)alloc_or_die(sizeof *h);
    h->task = t;
    h->lock = l;
    h->write = write;
    struct filc_async_hold** tail = &l->waiters;
    while (*tail)
        tail = &(*tail)->lock_next;
    *tail = h;

    bool waited = false;
    for (;;) {
        struct filc_async_task* holder = lock_conflict(l, t, write);
        bool queued_behind = false;
        for (struct filc_async_hold* w = l->waiters; w != h; w = w->lock_next)
            if (w->task != t && (write || w->write)) {
                queued_behind = true;
                break;
            }
        if (!holder && !queued_behind)
            break;
        if (!waited) {
            ++g_lock_waits;
            waited = true;
        }
        if (holder)
            wait_for(holder);
        else
            wait_changed();
    }

    struct filc_async_hold** link = &l->waiters;
    while (*link != h)
        link = &(*link)->lock_next;
    *link = h->lock_next;
    h->lock_next = l->holders;
    h->lock_pprev = &l->holders;
    if (l->holders)
        l->holders->lock_pprev = &h->lock_next;
    l->holders = h;
    ++l->nholders;
    if (write)
        ++l->nwriters;
    h->task_next = t->holds;
    t->holds = h;
    pthread_cond_broadcast(&g_changed);
}

/* Releases every lock `t` holds; a lock nobody holds goes back to the free
 * list. */
static void locks_release(struct filc_async_task* t)
{
    for (struct filc_async_hold* h = t->holds; h; h = h->task_next) {
        struct filc_async_lock* l = h->lock;
        *h->lock_pprev = h->lock_next;
        if (h->lock_next)
            h->lock_next->lock_pprev = h->lock_pprev;
        --l->nholders;
        if (h->write)
            --l->nwriters;
        if (l->holders || l->waiters)
            continue;
        /* Counts that drifted would grant a writer beside a reader, or keep
         * every request on the slow path: stop rather than run on. */
        if (l->nholders || l->nwriters)
            filc_async_fatal("dependency lock counts out of step");
        struct filc_async_lock** bucket = lock_bucket(l->value, l->space);
        while (*bucket != l)
            bucket = &(*bucket)->bucket_next;
        *bucket = l->bucket_next;
        l->bucket_next = g_free_locks;
        g_free_locks = l;
    }
    t->holds = NULL;
}

void filc_async_lock_word(void* task, uint64_t value, uint32_t space,
                          uint32_t mode)
{
    lock();
    lock_take((struct filc_async_task*)task, value,
              space & ~FILC_ASYNC_DEP_POINTER, mode);
    unlock();
}

/* A pointer is a key by object identity, so it locks the object's lower
 * bound. */
void filc_async_lock_ptr(void* task, const void* ptr, uint32_t space,
                         uint32_t mode)
{
    uint64_t value = (uint64_t)(uintptr_t)(ptr ? zgetlower((void*)ptr) : NULL);
    lock();
    lock_take((struct filc_async_task*)task, value,
              space | FILC_ASYNC_DEP_POINTER, mode);
    unlock();
}

/* ---- Pending buffers ----
 *
 * A mark covers the whole object, [zgetlower, zgetupper), and holds the real
 * buffer pointer so a resolve keeps its capability. Live objects never
 * overlap, so a mark that overlaps an object's range has that object's lower
 * bound: marks are hashed by it, and every lookup reads one bucket.
 *
 * A mark is a write mark (the call produces the object) or a read mark (the
 * call only reads it). While an object has a mark, its header carries the
 * pending flag, so the compiler's access hook calls in, and the program's
 * access waits for every call that owns the object: for its producers before
 * reading it, and for its readers too, so a store never changes what a
 * queued call is still reading. A read-only object, which the program cannot
 * store into, gets no flag for its read marks.
 *
 * An annotated call's write marks are exclusive: marking waits for every
 * other owner, readers included, so a write never overtakes a call still
 * reading the object. Read marks never wait, so calls that only read an
 * object run together. A runtime may add shared write marks, so several of
 * its requests can own one object; an access then waits for all of them.
 * Waiting for a buffer from a runtime (filc_async_wait_buffer), is_pending,
 * mark_resolved and wait_all consider producers only.
 *
 * Each lookup below walks the bucket, so N marks on one object make N calls
 * cost O(N^2) in all. */

#define MARK_BUCKET_BITS 11

typedef struct filc_async_mark {
    void* buf;
    uintptr_t lower;
    uintptr_t upper;
    uint64_t generation;
    struct filc_async_task* owner;
    struct filc_async_mark* bucket_next; /* same bucket */
    struct filc_async_mark* owner_next;  /* the owner's marks, or free list */
    unsigned char write;                 /* a write mark, not a read mark */
} filc_async_mark;

static filc_async_mark* g_free_marks;
static filc_async_mark* g_mark_buckets[1u << MARK_BUCKET_BITS];
static bool g_resolver_set;

static void resolve_access(void* object);

static filc_async_mark** mark_bucket(uintptr_t lower)
{
    return &g_mark_buckets[hash_bits(lower, MARK_BUCKET_BITS)];
}

/* Any mark on the object: whether some call still owns it. */
static filc_async_mark* mark_find(uintptr_t lower, uintptr_t upper)
{
    for (filc_async_mark* m = *mark_bucket(lower); m; m = m->bucket_next)
        if (lower < m->upper && m->lower < upper)
            return m;
    return NULL;
}

/* A write mark on the object: whether it is still being produced. */
static filc_async_mark* writer_find(uintptr_t lower, uintptr_t upper)
{
    for (filc_async_mark* m = *mark_bucket(lower); m; m = m->bucket_next)
        if (lower < m->upper && m->lower < upper && m->write)
            return m;
    return NULL;
}

/* A mark on the object that `t` does not own, if any. With no task, every
 * mark is another's, so re-marking without an owner replaces the old mark.
 * The read marks of the body running on this thread do not count: a call
 * that body makes must not wait for the body itself. */
static filc_async_mark* mark_find_other(uintptr_t lower, uintptr_t upper,
                                        const struct filc_async_task* t)
{
    for (filc_async_mark* m = *mark_bucket(lower); m; m = m->bucket_next)
        if (lower < m->upper && m->lower < upper && (!t || m->owner != t) &&
            (m->write || !m->owner || m->owner != g_running_body))
            return m;
    return NULL;
}

/* The mark `t` holds on the object, if any; a task holds at most one. */
static filc_async_mark* mark_owned(uintptr_t lower,
                                   const struct filc_async_task* t)
{
    for (filc_async_mark* m = *mark_bucket(lower); m; m = m->bucket_next)
        if (m->lower == lower && m->owner == t)
            return m;
    return NULL;
}

/* Makes a read mark a write mark. The flag is already set unless the object
 * is read-only and had only read marks. */
static void mark_make_write(filc_async_mark* m)
{
    if (m->write)
        return;
    if (!writer_find(m->lower, m->upper))
        zasync_set_pending(m->buf, PENDING_SET);
    m->write = 1;
}

static void mark_add(void* buf, uintptr_t lower, uintptr_t upper,
                     struct filc_async_task* owner, bool write)
{
    if (!g_resolver_set) {
        zasync_set_resolver(resolve_access);
        g_resolver_set = true;
    }
    /* A read mark needs the flag only if the object has no mark yet; a write
     * mark also if the object had only read marks and is read-only. */
    if (write ? !writer_find(lower, upper) : !mark_find(lower, upper))
        zasync_set_pending(buf, write ? PENDING_SET : PENDING_READERS);
    filc_async_mark* m = g_free_marks;
    if (m)
        g_free_marks = m->owner_next;
    else
        m = (filc_async_mark*)alloc_or_die(sizeof *m);
    if (m->generation == UINT64_MAX)
        filc_async_fatal("pending mark generation overflow");
    ++m->generation;
    m->buf = buf;
    m->lower = lower;
    m->upper = upper;
    m->owner = owner;
    m->write = write;
    m->owner_next = NULL;
    if (owner) {
        m->owner_next = owner->marks;
        owner->marks = m;
    }
    filc_async_mark** bucket = mark_bucket(lower);
    m->bucket_next = *bucket;
    *bucket = m;
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
    /* The last mark clears the flag. When the last write mark goes and
     * readers remain, the flag stays only if the object is writable. */
    if (!mark_find(m->lower, m->upper))
        zasync_set_pending(m->buf, PENDING_CLEAR);
    else if (m->write && !writer_find(m->lower, m->upper))
        zasync_set_pending(m->buf, PENDING_READERS);
    m->buf = NULL;
    m->owner = NULL;
    m->write = 0;
    m->owner_next = g_free_marks;
    g_free_marks = m;
    pthread_cond_broadcast(&g_changed);
}

void filc_async_mark_pending(void* task, void* buf)
{
    if (!buf)
        return;
    struct filc_async_task* t = (struct filc_async_task*)task;
    uintptr_t lower = (uintptr_t)zgetlower(buf);
    uintptr_t upper = (uintptr_t)zgetupper(buf);

    /* Waiting for another owner retires its mark, so the loop looks again
     * until only `t` holds the object. */
    lock();
    for (;;) {
        filc_async_mark* m = mark_find_other(lower, upper, t);
        if (!m)
            break;
        if (m->owner)
            wait_for(m->owner);
        else
            mark_remove(m);
        ++g_pending_resolves;
    }
    filc_async_mark* own = t ? mark_owned(lower, t) : NULL;
    if (own)
        mark_make_write(own);
    else
        mark_add(buf, lower, upper, t, true);
    unlock();
}

void filc_async_mark_shared(void* task, void* buf)
{
    if (!buf || !task)
        return;
    struct filc_async_task* t = (struct filc_async_task*)task;
    uintptr_t lower = (uintptr_t)zgetlower(buf);
    lock();
    filc_async_mark* own = mark_owned(lower, t);
    if (own)
        mark_make_write(own);
    else
        mark_add(buf, lower, (uintptr_t)zgetupper(buf), t, true);
    unlock();
}

void filc_async_mark_input(void* task, const void* buf)
{
    if (!buf || !task)
        return;
    struct filc_async_task* t = (struct filc_async_task*)task;
    uintptr_t lower = (uintptr_t)zgetlower((void*)buf);
    lock();
    if (!mark_owned(lower, t))
        mark_add((void*)buf, lower, (uintptr_t)zgetupper((void*)buf), t, false);
    unlock();
}

/* Waits until no call but `task`, and not the call whose body this thread is
 * running, is producing the object `buf` points into. Read marks and marks
 * without an owner have nothing to wait for. */
static void wait_buffer_locked(void* task, const void* buf)
{
    uintptr_t lower = (uintptr_t)zgetlower((void*)buf);
    uintptr_t upper = (uintptr_t)zgetupper((void*)buf);
    for (;;) {
        struct filc_async_task* owner = NULL;
        for (filc_async_mark* m = *mark_bucket(lower); m; m = m->bucket_next)
            if (lower < m->upper && m->lower < upper && m->write && m->owner &&
                m->owner != task && m->owner != g_running_body) {
                owner = m->owner;
                break;
            }
        if (!owner)
            return;
        wait_for(owner);
    }
}

void filc_async_wait_buffer(void* task, const void* buf)
{
    if (!buf)
        return;
    lock();
    wait_buffer_locked(task, buf);
    unlock();
}

/* The access hook's slow path: the program reached an object whose pending
 * flag is set. It waits for every call that owns the object, readers too:
 * the hook cannot tell a load from a store, and a store must not change what
 * a queued call is still reading. The calls whose body or submit runs on
 * this thread are skipped; waiting for them would wait for this thread. */
static void resolve_access(void* object)
{
    uintptr_t lower = (uintptr_t)zgetlower(object);
    uintptr_t upper = (uintptr_t)zgetupper(object);
    lock();
    ++g_hook_resolves;
    for (;;) {
        struct filc_async_task* owner = NULL;
        for (filc_async_mark* m = *mark_bucket(lower); m; m = m->bucket_next)
            if (lower < m->upper && m->lower < upper && m->owner &&
                m->owner != g_running_body && m->owner != g_submitting) {
                owner = m->owner;
                break;
            }
        if (!owner)
            break;
        wait_for(owner);
    }
    unlock();
}

void filc_async_mark_resolved(void* buf)
{
    if (!buf)
        return;
    uintptr_t lower = (uintptr_t)zgetlower(buf);
    lock();
    for (filc_async_mark* m = *mark_bucket(lower); m; m = m->bucket_next)
        if (m->lower == lower && m->write) {
            mark_remove(m);
            break;
        }
    unlock();
}

void filc_async_resolve_buffer(void* task, void* buf)
{
    if (!buf)
        return;
    uintptr_t lower = (uintptr_t)zgetlower(buf);
    lock();
    for (filc_async_mark* m = *mark_bucket(lower); m; m = m->bucket_next)
        if (m->lower == lower && m->owner == (struct filc_async_task*)task) {
            mark_remove(m);
            break;
        }
    unlock();
}

bool filc_async_is_pending(const void* buf)
{
    if (!buf)
        return false;
    uintptr_t lower = (uintptr_t)zgetlower((void*)buf);
    uintptr_t upper = (uintptr_t)zgetupper((void*)buf);
    lock();
    bool pending = writer_find(lower, upper) != NULL;
    unlock();
    return pending;
}

struct join_entry {
    filc_async_mark* mark;
    uint64_t generation;
};

struct join_state {
    size_t count;
    struct join_entry entries[];
};

static bool join_entry_pending(const struct join_entry* e)
{
    return e->mark->buf && e->mark->generation == e->generation;
}

// The join's own storage is GC memory rather than arena memory: the arena
// never frees, and a program may join in a loop. Nothing refers to it once
// the join completes.
static void* gc_alloc_or_die(size_t size)
{
    void* p = zgc_aligned_alloc(16, size);
    if (!p)
        filc_async_fatal("out of memory");
    return p;
}

static bool join_poll(void* task, enum filc_async_poll_mode mode)
{
    struct filc_async_task* t = (struct filc_async_task*)task;
    const struct join_state* s = (const struct join_state*)t->runtime_data;
    lock();
    while (t->state == 1) {
        for (size_t i = 0; i < s->count; ++i) {
            if (!join_entry_pending(&s->entries[i]))
                continue;
            struct filc_async_task* owner = s->entries[i].mark->owner;
            if (!owner)
                continue;
            // progress every producer without waiting for any one producer
            unlock();
            owner->runtime->poll(owner, mode == FILC_ASYNC_POLL_CHECK ?
                FILC_ASYNC_POLL_CHECK : FILC_ASYNC_POLL_PROGRESS);
            lock();
        }
        bool pending = false;
        for (size_t i = 0; i < s->count; ++i)
            if (join_entry_pending(&s->entries[i])) {
                pending = true;
                break;
            }
        if (!pending) {
            unlock();
            filc_async_complete(t, 0);
            // drop the snapshot as soon as the group is done
            lock();
            t->runtime_data = NULL;
            unlock();
            return true;
        }
        if (mode != FILC_ASYNC_POLL_BLOCK) {
            bool done = t->state != 1;
            unlock();
            return done;
        }
        // also wakes for early buffer resolution and ownerless marks
        wait_changed();
    }
    unlock();
    return true;
}

static const filc_async_runtime g_join_runtime = {
    "wait_all", NULL, join_poll, NULL
};

prov_tag prov_alloc(void)
{
    return zgc_aligned_alloc(16, 16);
}

prov_tag filc_async_wait_all(const prov_tag* buffers, size_t count)
{
    if ((!buffers && count) || count > SIZE_MAX / sizeof *buffers)
        filc_async_fatal("wait_all: invalid input array");
    const void** snapshot = count ? (const void**)gc_alloc_or_die(
        count * sizeof *snapshot) : NULL;
    for (size_t i = 0; i < count; ++i)
        snapshot[i] = buffers[i];
    prov_tag token = prov_alloc();
    if (!token)
        filc_async_fatal("out of memory");
    lock();
    size_t capacity = 0;
    const size_t max_entries =
        (SIZE_MAX - sizeof(struct join_state)) / sizeof(struct join_entry);
    for (size_t i = 0; i < count; ++i) {
        if (!snapshot[i])
            continue;
        uintptr_t lower = (uintptr_t)zgetlower((void*)snapshot[i]);
        for (filc_async_mark* m = *mark_bucket(lower); m; m = m->bucket_next)
            if (m->lower == lower && m->write) {
                if (capacity == max_entries)
                    filc_async_fatal("wait_all: too many pending marks");
                ++capacity;
            }
    }
    if (!capacity) {
        unlock();
        return token;
    }
    struct join_state* s = (struct join_state*)gc_alloc_or_die(
        sizeof *s + capacity * sizeof s->entries[0]);
    for (size_t i = 0; i < count; ++i) {
        if (!snapshot[i])
            continue;
        uintptr_t lower = (uintptr_t)zgetlower((void*)snapshot[i]);
        for (filc_async_mark* m = *mark_bucket(lower); m; m = m->bucket_next) {
            if (m->lower != lower || !m->write)
                continue;
            size_t j;
            for (j = 0; j < s->count; ++j)
                if (s->entries[j].mark == m)
                    break;
            if (j == s->count)
                s->entries[s->count++] = (struct join_entry){ m, m->generation };
        }
    }
    struct filc_async_task* t =
        (struct filc_async_task*)gc_alloc_or_die(sizeof *t);
    t->runtime = &g_join_runtime;
    t->runtime_data = s;
    t->state = 1;
    mark_add(token, (uintptr_t)zgetlower(token), (uintptr_t)zgetupper(token), t,
             true);
    // publish only after the snapshot and output mark are ready
    running_add(t);
    unlock();
    return token;
}

/* ---- Tasks ---- */

static struct filc_async_task* task_start(const filc_async_runtime* rt,
                                          const filc_async_meta* meta,
                                          void* staged_args)
{
    if (!rt)
        filc_async_fatal("a task has no runtime");
    struct filc_async_task* t =
        (struct filc_async_task*)alloc_or_die(sizeof *t);
    /* The allocator returns zeroed memory. */
    t->meta = meta;
    t->runtime = rt;
    t->staged_args = staged_args;
    t->state = 1;
    lock();
    running_add(t);
    handle_add(t);
    /* The counts cover annotated calls, not tasks a runtime starts itself. */
    if (meta)
        ++g_submitted;
    unlock();
    return t;
}

void* filc_async_begin(const filc_async_meta* meta, void* staged_args)
{
    if (!meta)
        filc_async_fatal("filc_async_begin: no descriptor");
    return task_start(meta->runtime, meta, staged_args);
}

void* filc_async_task_new(const filc_async_runtime* rt)
{
    return task_start(rt, NULL, NULL);
}

void filc_async_submit(void* task, const filc_async_meta* meta,
                       filc_async_run_fn run, void* staged_args, size_t nargs)
{
    /* While its runtime submits the call, an access to the call's own
     * buffers, such as io_uring reading an openat path, does not wait for
     * the call. */
    struct filc_async_task* outer = g_submitting;
    g_submitting = (struct filc_async_task*)task;
    meta->runtime->submit(task, meta, run, staged_args, nargs);
    g_submitting = outer;
}

void filc_async_complete(void* task, long result)
{
    struct filc_async_task* t = (struct filc_async_task*)task;
    if (!t)
        return;
    lock();
    if (t->state != 1) {
        unlock();
        return;
    }
    t->result = result;
    t->state = result < 0 ? 2 : 0;
    if (t->meta) {
        ++g_completed;
        if (result < 0)
            ++g_failed;
    }
    while (t->marks)
        mark_remove(t->marks);
    locks_release(t);
    running_remove(t);
    /* A runtime needs the staged arguments only until the call completes.
     * The arena never frees, and its blocks stay reachable for the life of
     * the process, so clear the cells: otherwise every buffer an annotated
     * call was ever given stays reachable, and the collector keeps scanning
     * them. */
    if (t->meta && t->staged_args) {
        memset(t->staged_args, 0, t->meta->nargs * 16);
        t->staged_args = NULL;
    }
    pthread_cond_broadcast(&g_changed);
    unlock();
}

long filc_async_run(void* task, filc_async_run_fn run, void* staged_args)
{
    if (!run)
        return 0;
    struct filc_async_task* outer = g_running_body;
    g_running_body = (struct filc_async_task*)task;
    long result = run(staged_args);
    g_running_body = outer;
    return result;
}

void** filc_async_task_runtime_data(void* task)
{
    return &((struct filc_async_task*)task)->runtime_data;
}

static void result_fill(struct filc_async_result_s* out,
                        const struct filc_async_task* t)
{
    out->result = t->result;
    out->state = t->state;
}

bool filc_async_poll(struct filc_async_result_s* out)
{
    if (!out || !out->pending)
        return false;
    lock();
    struct filc_async_task* t = find_task(out->pending);
    if (!t) {
        unlock();
        return false;
    }
    if (t->state == 1) {
        unlock();
        t->runtime->poll(t, FILC_ASYNC_POLL_PROGRESS);
        lock();
    }
    result_fill(out, t);
    bool done = t->state != 1;
    if (done)
        retire_task(t);
    unlock();
    return done;
}

void filc_async_wait(struct filc_async_result_s* out)
{
    if (!out || !out->pending)
        return;
    lock();
    struct filc_async_task* t = find_task(out->pending);
    if (t) {
        wait_for(t);
        result_fill(out, t);
        retire_task(t);
    }
    unlock();
}

/* Asks the runtime, without waiting, about every task still running, so the
 * counts include calls that finished without anyone asking. */
void filc_async_get_stats(filc_async_stats* out)
{
    if (!out)
        return;
    lock();
    for (struct filc_async_task* t = g_oldest; t; t = t->newer)
        if (t->state == 1) {
            unlock();
            t->runtime->poll(t, FILC_ASYNC_POLL_CHECK);
            lock();
        }
    *out = (filc_async_stats){ 0 };
    out->tasks_submitted = g_submitted;
    out->tasks_completed = g_completed;
    out->tasks_failed = g_failed;
    out->pending_resolves = g_pending_resolves;
    out->lock_waits = g_lock_waits;
    out->hook_resolves = g_hook_resolves;
    unlock();
}

/* ---- Startup validation ---- */

/* The dependency bits of every argument: a read or a write, the pointer bit,
 * and a namespace, and nothing else. */
static bool dependencies_ok(const filc_async_meta* m)
{
    if (m->noped_args > m->nargs)
        return false;
    for (size_t i = 0; i < m->nargs; ++i) {
        unsigned dep = m->args[i].dependency;
        unsigned mode = dep & (FILC_ASYNC_DEP_READ | FILC_ASYNC_DEP_WRITE);
        if (mode == (FILC_ASYNC_DEP_READ | FILC_ASYNC_DEP_WRITE))
            return false;
        if (dep & ~(FILC_ASYNC_DEP_POINTER | FILC_ASYNC_DEP_READ |
                    FILC_ASYNC_DEP_WRITE |
                    (FILC_ASYNC_DEP_NAMESPACE_MASK
                     << FILC_ASYNC_DEP_NAMESPACE_SHIFT)))
            return false;
        if (mode == FILC_ASYNC_DEP_NONE && dep != FILC_ASYNC_DEP_NONE)
            return false;
    }
    return true;
}

void filc_async_set_validator(filc_async_validator_fn fn)
{
    g_validator = fn;
}

void filc_async_validate_table(const filc_async_meta* const* metas)
{
    for (const filc_async_meta* const* p = metas; p && *p; ++p) {
        const filc_async_meta* m = *p;
        if (!m->runtime)
            filc_async_fatal("function names no runtime");
        bool ok = g_validator ? g_validator(m) : m->runtime->validate(m);
        if (!dependencies_ok(m) || !ok)
            filc_async_fatal("function cannot be registered on this runtime");
    }
}

void filc_async_fatal(const char* msg)
{
    // Runtime objects in libpizlo.a cannot reach pizlonated libc (fprintf/
    // abort): the driver links `-lc` BEFORE `-lpizlo`. Go through the zsys
    // native thunks our own archive exports instead.
    static const char prefix[] = "filc_async: fatal: ";
    zsys_write(2, prefix, sizeof(prefix) - 1);
    for (const char* p = msg; *p; ++p)
        zsys_write(2, p, 1);
    zsys_write(2, "\n", 1);
    zsys_abort();
    while (1) { }
}
