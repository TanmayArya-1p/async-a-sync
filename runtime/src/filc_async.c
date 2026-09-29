#include <errno.h>
#include <pizlonated_syscalls.h>
#include <stdfil.h>

#include "filc_async.h"
#include "filc_async_alloc.h"
#include "filc_async_runtime.h"

/* The generic framework behind annotated calls. The pass-emitted stub of each
 * annotated function calls filc_async_begin, takes the call's dependency
 * locks, marks its output buffers, and hands the call to the runtime with
 * filc_async_submit. This file knows nothing about any op or runtime: it
 * reaches the runtime only through filc_async_runtime.h.
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
 * Tasks, marks and locks live in the allocator arena. */

struct filc_async_task;
struct filc_async_mark;
struct filc_async_hold;

struct filc_async_task {
    const filc_async_meta* meta;
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

/* The task whose body filc_async_run is running. */
static struct filc_async_task* g_running_body;

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

/* Blocks until the runtime has completed `t`. */
static void wait_for(struct filc_async_task* t)
{
    while (t->state == 1)
        filc_async_runtime_poll(t, FILC_ASYNC_POLL_BLOCK);
}

/* ---- Dependency locks ----
 *
 * One lock per (value, space), where space is the argument's pointer bit and
 * namespace. A call holding a read lock shares it with other readers; a
 * write lock is exclusive. A call that needs a lock another call holds in a
 * conflicting mode waits in its stub, by polling that call through the
 * runtime, until it completes and releases the lock. Stubs run in program
 * order, so the locks are granted in submission order. */

struct filc_async_lock {
    struct filc_async_lock* bucket_next; /* same bucket, or free list */
    uint64_t value;
    uint32_t space;
    struct filc_async_hold* holders;
};

struct filc_async_hold {
    struct filc_async_task* task;
    struct filc_async_lock* lock;
    struct filc_async_hold* task_next;  /* the task's holds */
    struct filc_async_hold* lock_next;  /* the lock's holders */
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
    l->bucket_next = *bucket;
    *bucket = l;
    return l;
}

/* A holder of `l` that `t` must wait for before taking it in `write` mode. */
static struct filc_async_task* lock_conflict(const struct filc_async_lock* l,
                                             const struct filc_async_task* t,
                                             bool write)
{
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
 * 0xFFFFFFFFFFFFFFFE as a `long fd` argument, so a w_dep=0 call taking `int`
 * and an r_dep=0 call taking `long` on that handle are not ordered.
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
    for (;;) {
        struct filc_async_lock* l = lock_get(value, space);
        struct filc_async_task* holder = lock_conflict(l, t, write);
        if (!holder) {
            struct filc_async_hold* h =
                (struct filc_async_hold*)alloc_or_die(sizeof *h);
            h->task = t;
            h->lock = l;
            h->write = write;
            h->lock_next = l->holders;
            l->holders = h;
            h->task_next = t->holds;
            t->holds = h;
            return;
        }
        ++g_lock_waits;
        wait_for(holder);
    }
}

/* Releases every lock `t` holds; a lock nobody holds goes back to the free
 * list. */
static void locks_release(struct filc_async_task* t)
{
    for (struct filc_async_hold* h = t->holds; h; h = h->task_next) {
        struct filc_async_lock* l = h->lock;
        struct filc_async_hold** link = &l->holders;
        while (*link != h)
            link = &(*link)->lock_next;
        *link = h->lock_next;
        if (l->holders)
            continue;
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
    lock_take((struct filc_async_task*)task, value,
              space & ~FILC_ASYNC_DEP_POINTER, mode);
}

/* A pointer is a key by object identity, so it locks the object's lower
 * bound. */
void filc_async_lock_ptr(void* task, const void* ptr, uint32_t space,
                         uint32_t mode)
{
    uint64_t value = (uint64_t)(uintptr_t)(ptr ? zgetlower((void*)ptr) : NULL);
    lock_take((struct filc_async_task*)task, value,
              space | FILC_ASYNC_DEP_POINTER, mode);
}

/* ---- Pending buffers ----
 *
 * A mark covers the whole object, [zgetlower, zgetupper), and holds the real
 * buffer pointer so a resolve keeps its capability. Live objects never
 * overlap, so a mark that overlaps an object's range has that object's lower
 * bound: marks are hashed by it, and every lookup reads one bucket. */

#define MARK_BUCKET_BITS 11

typedef struct filc_async_mark {
    void* buf;
    uintptr_t lower;
    uintptr_t upper;
    struct filc_async_task* owner;
    struct filc_async_mark* bucket_next; /* same bucket */
    struct filc_async_mark* owner_next;  /* the owner's marks, or free list */
} filc_async_mark;

static filc_async_mark* g_free_marks;
static filc_async_mark* g_mark_buckets[1u << MARK_BUCKET_BITS];

static filc_async_mark** mark_bucket(uintptr_t lower)
{
    return &g_mark_buckets[hash_bits(lower, MARK_BUCKET_BITS)];
}

static filc_async_mark* mark_find(uintptr_t lower, uintptr_t upper)
{
    for (filc_async_mark* m = *mark_bucket(lower); m; m = m->bucket_next)
        if (lower < m->upper && m->lower < upper)
            return m;
    return NULL;
}

static void mark_add(void* buf, uintptr_t lower, uintptr_t upper,
                     struct filc_async_task* owner)
{
    filc_async_mark* m = g_free_marks;
    if (m)
        g_free_marks = m->owner_next;
    else
        m = (filc_async_mark*)alloc_or_die(sizeof *m);
    m->buf = buf;
    m->lower = lower;
    m->upper = upper;
    m->owner = owner;
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
    m->buf = NULL;
    m->owner = NULL;
    m->owner_next = g_free_marks;
    g_free_marks = m;
}

void filc_async_mark_pending(void* task, void* buf)
{
    if (!buf)
        return;
    struct filc_async_task* t = (struct filc_async_task*)task;
    uintptr_t lower = (uintptr_t)zgetlower(buf);
    uintptr_t upper = (uintptr_t)zgetupper(buf);

    /* Waiting for the owner of an overlapping mark retires that mark, so the
     * loop looks again until the buffer can be marked. */
    for (;;) {
        filc_async_mark* m = mark_find(lower, upper);
        if (!m)
            break;
        if (m->owner == t && t)
            return;
        if (m->owner) {
            wait_for(m->owner);
        } else {
            mark_remove(m);
        }
        ++g_pending_resolves;
    }
    mark_add(buf, lower, upper, t);
}

void filc_async_mark_resolved(void* buf)
{
    if (!buf)
        return;
    uintptr_t lower = (uintptr_t)zgetlower(buf);
    for (filc_async_mark* m = *mark_bucket(lower); m; m = m->bucket_next)
        if (m->lower == lower) {
            mark_remove(m);
            return;
        }
}

void filc_async_resolve_buffer(void* task, void* buf)
{
    if (!buf)
        return;
    uintptr_t lower = (uintptr_t)zgetlower(buf);
    for (filc_async_mark* m = *mark_bucket(lower); m; m = m->bucket_next)
        if (m->lower == lower && m->owner == (struct filc_async_task*)task) {
            mark_remove(m);
            return;
        }
}

bool filc_async_is_pending(const void* buf)
{
    if (!buf)
        return false;
    uintptr_t lower = (uintptr_t)zgetlower((void*)buf);
    uintptr_t upper = (uintptr_t)zgetupper((void*)buf);
    filc_async_mark* m = mark_find(lower, upper);
    /* The owner may have finished without anyone asking; ask the runtime
     * without waiting before reporting. */
    if (m && m->owner && m->owner->state == 1) {
        filc_async_runtime_poll(m->owner, FILC_ASYNC_POLL_CHECK);
        m = mark_find(lower, upper);
    }
    return m != NULL;
}

/* ---- Tasks ---- */

void* filc_async_begin(const filc_async_meta* meta, void* staged_args)
{
    if (!meta || !staged_args)
        filc_async_fatal("filc_async_begin: bad call");
    struct filc_async_task* t =
        (struct filc_async_task*)alloc_or_die(sizeof *t);
    /* The allocator returns zeroed memory. */
    t->meta = meta;
    t->staged_args = staged_args;
    t->state = 1;
    running_add(t);
    handle_add(t);
    ++g_submitted;
    return t;
}

void filc_async_complete(void* task, long result)
{
    struct filc_async_task* t = (struct filc_async_task*)task;
    if (!t || t->state != 1)
        return;
    t->result = result;
    t->state = result < 0 ? 2 : 0;
    ++g_completed;
    if (result < 0)
        ++g_failed;
    while (t->marks)
        mark_remove(t->marks);
    locks_release(t);
    running_remove(t);
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
    struct filc_async_task* t = find_task(out->pending);
    if (!t)
        return false;
    if (t->state == 1)
        filc_async_runtime_poll(t, FILC_ASYNC_POLL_PROGRESS);
    result_fill(out, t);
    if (t->state == 1)
        return false;
    retire_task(t);
    return true;
}

void filc_async_wait(struct filc_async_result_s* out)
{
    if (!out || !out->pending)
        return;
    struct filc_async_task* t = find_task(out->pending);
    if (!t)
        return;
    wait_for(t);
    result_fill(out, t);
    retire_task(t);
}

/* Asks the runtime, without waiting, about every task still running, so the
 * counts include calls that finished without anyone asking. */
void filc_async_get_stats(filc_async_stats* out)
{
    if (!out)
        return;
    for (struct filc_async_task* t = g_oldest; t; t = t->newer)
        if (t->state == 1)
            filc_async_runtime_poll(t, FILC_ASYNC_POLL_CHECK);
    *out = (filc_async_stats){ 0 };
    out->tasks_submitted = g_submitted;
    out->tasks_completed = g_completed;
    out->tasks_failed = g_failed;
    out->pending_resolves = g_pending_resolves;
    out->lock_waits = g_lock_waits;
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
    filc_async_validator_fn validator =
        g_validator ? g_validator : filc_async_runtime_validate;
    for (const filc_async_meta* const* p = metas; p && *p; ++p) {
        const filc_async_meta* m = *p;
        if (!dependencies_ok(m) || !validator(m))
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
