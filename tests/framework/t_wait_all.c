#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "filc_async.h"
#include "filc_async_runtime.h"

#ifdef HOST_MOCK
static struct { void* ptr; size_t size; } objects[512];
static _Atomic size_t object_count;

static void* object_alloc(size_t size)
{
    void* p = malloc(size);
    assert(p);
    size_t n = atomic_load(&object_count);
    assert(n < sizeof objects / sizeof objects[0]);
    objects[n].ptr = p;
    objects[n].size = size;
    atomic_store(&object_count, n + 1);
    return p;
}

void* zgetlower(void* p)
{
    uintptr_t v = (uintptr_t)p;
    for (size_t i = 0, n = atomic_load(&object_count); i < n; ++i)
        if (v >= (uintptr_t)objects[i].ptr &&
            v < (uintptr_t)objects[i].ptr + objects[i].size)
            return objects[i].ptr;
    abort();
}

void* zgetupper(void* p)
{
    void* lower = zgetlower(p);
    for (size_t i = 0, n = atomic_load(&object_count); i < n; ++i)
        if (lower == objects[i].ptr)
            return (char*)lower + objects[i].size;
    abort();
}

void* zgc_aligned_alloc(size_t align, size_t size)
{
    (void)align;
    return object_alloc(size);
}

void* filc_async_alloc(size_t size, size_t align)
{
    (void)align;
    return calloc(1, size);
}

void zasync_set_pending(void* p, int pending) { (void)p; (void)pending; }
void zasync_set_resolver(void (*resolver)(void*)) { (void)resolver; }
long zsys_write(int fd, const void* p, size_t len)
{
    return (long)fwrite(p, 1, len, fd == 2 ? stderr : stdout);
}
void zsys_abort(void) { abort(); }
#else
static void* object_alloc(size_t size)
{
    void* p = malloc(size);
    assert(p);
    return p;
}
#endif

struct producer {
    pthread_mutex_t mutex;
    _Atomic bool ready;
    _Atomic bool started;
    _Atomic bool completed;
    _Atomic unsigned checks;
    _Atomic unsigned progress;
    _Atomic unsigned blocks;
    _Atomic bool waiters[3];
    void* token;
    long result;
    bool early;
};

static _Thread_local unsigned waiter_id;

static bool producer_poll(void* task, enum filc_async_poll_mode mode)
{
    struct producer* p = *filc_async_task_runtime_data(task);
    if (waiter_id)
        atomic_store(&p->waiters[waiter_id], true);
    pthread_mutex_lock(&p->mutex);
    if (mode == FILC_ASYNC_POLL_CHECK)
        atomic_fetch_add(&p->checks, 1);
    else {
        atomic_store(&p->started, true);
        atomic_fetch_add(mode == FILC_ASYNC_POLL_BLOCK ? &p->blocks : &p->progress, 1);
    }
    if (atomic_load(&p->ready) && !atomic_load(&p->completed)) {
        if (p->early)
            filc_async_resolve_buffer(task, p->token);
        else {
            atomic_store(&p->completed, true);
            filc_async_complete(task, p->result);
        }
    }
    bool done = atomic_load(&p->completed);
    pthread_mutex_unlock(&p->mutex);
    return done;
}

static const filc_async_runtime producer_runtime = {
    "producer", NULL, producer_poll, NULL
};

static const filc_async_runtime other_runtime = {
    "other", NULL, producer_poll, NULL
};

static void* producer_new_on(struct producer* p, void* token, bool shared,
                            const filc_async_runtime* rt)
{
    *p = (struct producer){ .token = token };
    assert(!pthread_mutex_init(&p->mutex, NULL));
    void* task = filc_async_task_new(rt);
    *filc_async_task_runtime_data(task) = p;
    if (shared)
        filc_async_mark_shared(task, token);
    else
        filc_async_mark_pending(task, token);
    return task;
}

static void* producer_new(struct producer* p, void* token, bool shared)
{
    return producer_new_on(p, token, shared, &producer_runtime);
}

static void refresh(void)
{
    filc_async_stats stats;
    filc_async_get_stats(&stats);
}

struct waiter {
    void* token;
    unsigned id;
    _Atomic bool entered;
    _Atomic bool returned;
};

static void* wait_thread(void* arg)
{
    struct waiter* w = arg;
    waiter_id = w->id;
    atomic_store(&w->entered, true);
    filc_async_wait_buffer(NULL, w->token);
    atomic_store(&w->returned, true);
    return NULL;
}

static void until_true(const _Atomic bool* value)
{
    struct timespec pause = { .tv_nsec = 1000000 };
    for (unsigned i = 0; i < 10000; ++i) {
        if (atomic_load(value))
            return;
        nanosleep(&pause, NULL);
    }
    assert(!"timed out");
}

static void finish_task(void* task)
{
    struct filc_async_result_s r = { .pending = task };
    filc_async_wait(&r);
    assert(r.state == 0 || r.state == 2);
}

static void basic_cases(void)
{
    void* empty = filc_async_wait_all(NULL, 0);
    assert(!filc_async_is_pending(empty));
    void* clear = object_alloc(16);
    void* group = filc_async_wait_all((prov_tag[]){ NULL, clear }, 2);
    assert(group != empty && group != clear && !filc_async_is_pending(group));

    struct producer a, b;
    void* ta = producer_new(&a, object_alloc(16), false);
    void* tb = producer_new_on(&b, object_alloc(16), false, &other_runtime);
    const void* inputs[] = { a.token, b.token, a.token, NULL };
    group = filc_async_wait_all(inputs, 4);
    inputs[0] = inputs[1] = NULL;
    assert(filc_async_is_pending(group));
    refresh();
    assert(!atomic_load(&a.started) && !atomic_load(&b.started));
    atomic_store(&b.ready, true);
    refresh();
    assert(filc_async_is_pending(group));
    a.result = -EIO;
    atomic_store(&a.ready, true);
    filc_async_wait_buffer(NULL, group);
    assert(!filc_async_is_pending(group));
    struct filc_async_result_s failure = { .pending = ta };
    filc_async_wait(&failure);
    assert(failure.state == 2 && failure.result == -EIO);
    finish_task(tb);
    assert(!atomic_load(&a.blocks) && !atomic_load(&b.blocks));
}

static void aliases_shared_and_reuse(void)
{
    void* token = object_alloc(16);
    struct producer a, b;
    void* ta = producer_new(&a, token, true);
    void* tb = producer_new(&b, token, true);
    void* group = filc_async_wait_all(
        (prov_tag[]){ token, (char*)token + 1, token }, 3);
    atomic_store(&a.ready, true);
    refresh();
    assert(filc_async_is_pending(group));
    atomic_store(&b.ready, true);
    filc_async_wait_buffer(NULL, group);
    assert(!filc_async_is_pending(group));
    finish_task(ta);
    finish_task(tb);

    struct producer first, later;
    ta = producer_new(&first, token, false);
    group = filc_async_wait_all((prov_tag[]){ token }, 1);
    atomic_store(&first.ready, true);
    assert(producer_poll(ta, FILC_ASYNC_POLL_PROGRESS));
    tb = producer_new(&later, token, false);
    filc_async_wait_buffer(NULL, group);
    assert(!filc_async_is_pending(group) && filc_async_is_pending(token));
    assert(!atomic_load(&later.started));
    atomic_store(&later.ready, true);
    finish_task(ta);
    finish_task(tb);

    // even the same owner can remove and replace a mark
    struct producer again;
    ta = producer_new(&again, token, false);
    group = filc_async_wait_all((prov_tag[]){ token }, 1);
    filc_async_resolve_buffer(ta, token);
    filc_async_mark_pending(ta, token);
    filc_async_wait_buffer(NULL, group);
    assert(!filc_async_is_pending(group) && filc_async_is_pending(token));
    assert(!atomic_load(&again.started));
    atomic_store(&again.ready, true);
    finish_task(ta);
}

static void nested_and_early(void)
{
    struct producer a, b;
    void* ta = producer_new(&a, object_alloc(16), false);
    void* tb = producer_new(&b, object_alloc(16), false);
    void* inner = filc_async_wait_all((prov_tag[]){ a.token }, 1);
    void* outer = filc_async_wait_all((prov_tag[]){ inner, b.token }, 2);
    a.early = true;
    atomic_store(&a.ready, true);
    atomic_store(&b.ready, true);
    filc_async_wait_buffer(NULL, outer);
    assert(!filc_async_is_pending(inner) && !filc_async_is_pending(outer));
    assert(!atomic_load(&a.completed));
    filc_async_complete(ta, 0);
    atomic_store(&a.completed, true);
    finish_task(ta);
    finish_task(tb);

    // a blocking join observes early release while the producer is running
    assert(!pthread_mutex_destroy(&a.mutex));
    ta = producer_new(&a, object_alloc(16), false);
    a.early = true;
    struct waiter w = {
        .token = filc_async_wait_all((prov_tag[]){ a.token }, 1)
    };
    pthread_t thread;
    assert(!pthread_create(&thread, NULL, wait_thread, &w));
    until_true(&a.started);
    assert(!atomic_load(&w.returned));
    atomic_store(&a.ready, true);
    until_true(&w.returned);
    assert(!pthread_join(thread, NULL));
    assert(!atomic_load(&a.completed));
    filc_async_complete(ta, 0);
    atomic_store(&a.completed, true);
    finish_task(ta);
}

static void manual_and_concurrent(void)
{
    void* token = object_alloc(16);
    filc_async_mark_pending(NULL, token);
    struct producer p;
    void* task = producer_new(&p, object_alloc(16), false);
    void* group = filc_async_wait_all((prov_tag[]){ token, p.token }, 2);
    struct waiter a = { .token = group, .id = 1 }, b = { .token = group, .id = 2 };
    pthread_t threads[2];
    assert(!pthread_create(&threads[0], NULL, wait_thread, &a));
    assert(!pthread_create(&threads[1], NULL, wait_thread, &b));
    until_true(&a.entered);
    until_true(&b.entered);
    until_true(&p.waiters[1]);
    until_true(&p.waiters[2]);
    refresh();
    assert(filc_async_is_pending(group));
    assert(!atomic_load(&a.returned) && !atomic_load(&b.returned));
    filc_async_mark_resolved(token);
    refresh();
    assert(filc_async_is_pending(group));
    atomic_store(&p.ready, true);
    until_true(&a.returned);
    until_true(&b.returned);
    assert(!pthread_join(threads[0], NULL));
    assert(!pthread_join(threads[1], NULL));
    assert(!filc_async_is_pending(group));
    finish_task(task);
}

static struct producer reads[30];
static _Atomic bool writer_submitted;
static filc_async_meta* writer_meta;
static void* writer_task;

static void writer_submit(void* task, const filc_async_meta* meta,
                          filc_async_run_fn run, void* args, size_t nargs)
{
    (void)meta; (void)run; (void)args; (void)nargs;
    for (unsigned i = 0; i < 30; ++i)
        assert(atomic_load(&reads[i].completed));
    atomic_store(&writer_submitted, true);
    filc_async_complete(task, 0);
}

static bool writer_poll(void* task, enum filc_async_poll_mode mode)
{
    (void)task; (void)mode;
    return atomic_load(&writer_submitted);
}

static const filc_async_runtime writer_runtime = {
    "writer", writer_submit, writer_poll, NULL
};

static void* writer_thread(void* group)
{
    writer_task = filc_async_begin(writer_meta, NULL);
    filc_async_mark_pending(writer_task, group);
    filc_async_submit(writer_task, writer_meta, NULL, NULL, 0);
    return NULL;
}

static void thirty_reads_then_write(void)
{
    void* tasks[30];
    prov_tag tokens[30];
    for (unsigned i = 0; i < 30; ++i) {
        tokens[i] = object_alloc(16);
        tasks[i] = producer_new(&reads[i], (void*)tokens[i], false);
    }
    void* group = filc_async_wait_all(tokens, 30);
    writer_meta = calloc(1, sizeof *writer_meta);
    assert(writer_meta);
    writer_meta->runtime = &writer_runtime;
    pthread_t thread;
    assert(!pthread_create(&thread, NULL, writer_thread, group));
    for (unsigned i = 0; i < 30; ++i)
        until_true(&reads[i].started);
    assert(!atomic_load(&writer_submitted));
    for (unsigned i = 29; i > 0; --i) {
        atomic_store(&reads[i].ready, true);
        until_true(&reads[i].completed);
        assert(!atomic_load(&writer_submitted));
    }
    atomic_store(&reads[0].ready, true);
    until_true(&writer_submitted);
    assert(!pthread_join(thread, NULL));
    assert(!filc_async_is_pending(group));
    for (unsigned i = 0; i < 30; ++i) {
        assert(!atomic_load(&reads[i].blocks));
        finish_task(tasks[i]);
    }
    finish_task(writer_task);
}

int main(void)
{
    basic_cases();
    aliases_shared_and_reuse();
    nested_and_early();
    manual_and_concurrent();
    thirty_reads_then_write();
    puts("WAIT_ALL PASS (30 overlapping producers, ordered handoff, snapshots, nested groups, early release, concurrent waiters)");
    return 0;
}
