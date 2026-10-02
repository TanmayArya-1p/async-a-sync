/* Read marks against a mock runtime: a call that writes a buffer waits for
 * every earlier call still reading it, and so does the program's own access
 * to the buffer, while calls that only read it never wait for each other.
 * Calls complete only when polled with BLOCK or finished by hand, so each
 * wait is observable. Built with the host compiler against the framework
 * source, as check_dependencies is. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "filc_async.h"
#include "filc_async_alloc.h"
#include "filc_async_runtime.h"

typedef union {
    void* ptr;
    uint64_t word;
} staged_arg;

/* ---- Fil-C stand-ins: a fixed set of objects, each with a pending flag ---- */

#define NOBJ 16
#define OBJ_SIZE 64
static char objects[NOBJ][OBJ_SIZE];
static int flag[NOBJ];
static int readonly[NOBJ]; /* the program cannot store into these */
static void (*resolver)(void*);

static int object_index(const void* ptr)
{
    uintptr_t p = (uintptr_t)ptr;
    for (int i = 0; i < NOBJ; ++i)
        if (p >= (uintptr_t)objects[i] && p < (uintptr_t)objects[i] + OBJ_SIZE)
            return i;
    return -1;
}

void* zgetlower(void* ptr)
{
    int i = object_index(ptr);
    return i < 0 ? ptr : objects[i];
}

void* zgetupper(void* ptr)
{
    int i = object_index(ptr);
    return i < 0 ? (char*)ptr + 16 : objects[i] + OBJ_SIZE;
}

void zasync_set_pending(void* buf, int pending)
{
    int i = object_index(buf);
    /* 2 sets the flag for readers only, which a read-only object never needs */
    if (i >= 0)
        flag[i] = pending == 1 || (pending == 2 && !readonly[i]);
}

void zasync_set_resolver(void (*fn)(void*))
{
    resolver = fn;
}

long zsys_write(int fd, const void* buf, size_t len)
{
    return (long)fwrite(buf, 1, len, fd == 2 ? stderr : stdout);
}

void zsys_abort(void) { abort(); }

void* filc_async_alloc(size_t size, size_t align)
{
    (void)align;
    return calloc(1, size);
}

void* zgc_aligned_alloc(size_t alignment, size_t size)
{
    (void)alignment;
    return calloc(1, size);
}

/* What the compiler's access hook does for an instrumented access. */
static void host_access(void* buf)
{
    int i = object_index(buf);
    if (i >= 0 && flag[i] && resolver)
        resolver(buf);
}

/* ---- The mock runtime: numbers each call, runs its body at submit, and
 * completes call n only once done[n] is set or someone blocks on it. ---- */

#define MAX_CALLS 512
static unsigned issued;
static unsigned char done[MAX_CALLS];

static unsigned id_of(void* task)
{
    return (unsigned)(uintptr_t)*filc_async_task_runtime_data(task);
}

static filc_async_meta* toucher;
static filc_async_meta* late_writer; /* body runs at completion */

static filc_async_run_fn late_run[MAX_CALLS];
static void* late_args[MAX_CALLS];

static void mock_submit(void* task, const filc_async_meta* meta,
                        filc_async_run_fn run, void* staged_args, size_t nargs)
{
    (void)nargs;
    if (meta == late_writer) {
        assert(issued + 1 < MAX_CALLS);
        late_run[issued + 1] = run;
        late_args[issued + 1] = staged_args;
        *filc_async_task_runtime_data(task) = (void*)(uintptr_t)++issued;
        return;
    }
    filc_async_run(task, run, staged_args);
    /* Like io_uring scanning an openat path: the runtime reads the call's
     * own input while it submits the call. */
    if (meta == toucher)
        host_access(((staged_arg*)staged_args)[1].ptr);
    assert(issued + 1 < MAX_CALLS);
    *filc_async_task_runtime_data(task) = (void*)(uintptr_t)++issued;
}

static bool mock_poll(void* task, enum filc_async_poll_mode mode)
{
    unsigned id = id_of(task);
    if (!id)
        return false;
    if (!done[id]) {
        if (mode != FILC_ASYNC_POLL_BLOCK)
            return false;
        done[id] = 1;
    }
    if (late_run[id]) {
        /* a body that runs on completion, as a pool worker's would */
        filc_async_run_fn run = late_run[id];
        late_run[id] = NULL;
        filc_async_run(task, run, late_args[id]);
    }
    filc_async_complete(task, 0);
    return true;
}

static bool mock_validate(const filc_async_meta* meta)
{
    (void)meta;
    return true;
}

FILC_ASYNC_RUNTIME(mock, mock_submit, mock_poll, mock_validate);

static filc_async_meta* make_meta(const char* name, uint32_t kind)
{
    filc_async_meta* m = filc_async_alloc(sizeof *m + 2 * sizeof m->args[0], 16);
    assert(m);
    m->name = name;
    m->runtime = &filc_async_runtime_mock;
    m->nargs = 2;
    m->result = FILC_ASYNC_RESULT_PTR;
    m->args[0].kind = FILC_ASYNC_ARG_IGNORED;
    m->args[1].kind = kind;
    return m;
}

static filc_async_meta* reader;
static filc_async_meta* writer;
static filc_async_meta* nested;

/* What a pass-emitted stub does: start the task, mark outputs pending, give
 * inputs a read mark, hand the call to the runtime. */
static void* stub(const filc_async_meta* m, filc_async_run_fn run, void* buf)
{
    staged_arg* a = filc_async_alloc(2 * sizeof *a, 16);
    assert(a);
    a[1].ptr = buf;
    void* task = filc_async_begin(m, a);
    if (m->args[1].kind == FILC_ASYNC_ARG_BUFFER_OUT ||
        m->args[1].kind == FILC_ASYNC_ARG_PENDING)
        filc_async_mark_pending(task, buf);
    if (m->args[1].kind == FILC_ASYNC_ARG_BUFFER_IN)
        filc_async_mark_input(task, buf);
    filc_async_submit(task, m, run, a, 2);
    return task;
}

static void* read_call(void* buf) { return stub(reader, NULL, buf); }
static void* write_call(void* buf) { return stub(writer, NULL, buf); }

static void finish(void* task)
{
    done[id_of(task)] = 1;
    struct filc_async_result_s r = { .pending = task };
    assert(filc_async_poll(&r));
    assert(r.state == 0);
}

static unsigned long pending_resolves(void)
{
    filc_async_stats s;
    filc_async_get_stats(&s);
    return s.pending_resolves;
}

/* The body of a call that reads its buffer and, from inside the body, makes
 * a call that writes it. */
static void* nested_inner;
static long nested_body(void* staged)
{
    nested_inner = write_call(((staged_arg*)staged)[1].ptr);
    return 0;
}

/* The body of a call that reads its own input buffer. */
static long touching_body(void* staged)
{
    host_access(((staged_arg*)staged)[1].ptr);
    return 0;
}

/* A body that, from inside, makes a call whose own body touches the buffer,
 * and a call whose body writes it: two levels deep. */
static void* inner_reader;
static void* inner_writer;
static void* middle_call;
static long middle_body(void* staged)
{
    inner_writer = write_call(((staged_arg*)staged)[1].ptr);
    return 0;
}
static long outer_body(void* staged)
{
    void* buf = ((staged_arg*)staged)[1].ptr;
    inner_reader = stub(reader, touching_body, buf);
    middle_call = stub(reader, middle_body, buf);
    return 0;
}

int main(void)
{
    alarm(20); /* a wait that never ends fails the test instead of hanging it */
    reader = make_meta("reader", FILC_ASYNC_ARG_BUFFER_IN);
    writer = make_meta("writer", FILC_ASYNC_ARG_BUFFER_OUT);
    nested = make_meta("nested", FILC_ASYNC_ARG_BUFFER_IN);
    toucher = make_meta("toucher", FILC_ASYNC_ARG_BUFFER_IN);
    late_writer = make_meta("late_writer", FILC_ASYNC_ARG_BUFFER_OUT);
    const filc_async_meta* table[] = { reader, writer, nested, toucher,
                                       late_writer, NULL };
    filc_async_validate_table(table);

    /* A read mark sets the flag, so the program's accesses call in, but the
     * buffer is not pending: nothing is producing it. */
    void* r = read_call(objects[0]);
    assert(flag[0] && !filc_async_is_pending(objects[0]));
    /* A later writer waits for the reader before it reaches the runtime. */
    unsigned long before = pending_resolves();
    unsigned r_id = id_of(r);
    void* w = write_call(objects[0]);
    assert(done[r_id] && pending_resolves() == before + 1);
    assert(flag[0] && filc_async_is_pending(objects[0]));
    finish(w);
    assert(!flag[0]);

    /* Two readers of one buffer never wait for each other. */
    before = pending_resolves();
    void* r1 = read_call(objects[1]);
    void* r2 = read_call(objects[1] + 8);
    assert(!done[id_of(r1)] && !done[id_of(r2)] && pending_resolves() == before);
    assert(flag[1]);
    /* A runtime about to hand the buffer to the kernel waits for producers
     * only, so the readers do not wait on each other there either. */
    filc_async_wait_buffer(r2, objects[1]);
    filc_async_wait_buffer(NULL, objects[1]);
    assert(!done[id_of(r1)] && !done[id_of(r2)]);
    finish(r1);
    finish(r2);

    /* A reader never waits at marking, even behind a writer; the runtime's
     * wait for producers still orders it after the writer. */
    void* w2 = write_call(objects[2]);
    void* r3 = read_call(objects[2]);
    assert(!done[id_of(w2)]);
    filc_async_wait_buffer(r3, objects[2]);
    assert(done[id_of(w2)]);
    /* The writer has gone; the reader keeps the flag, though nothing is
     * producing the buffer any more. */
    assert(flag[2] && !filc_async_is_pending(objects[2]));
    /* The program's access waits for the reader: a store must not change
     * what a queued call is still reading. The last mark clears the flag. */
    host_access(objects[2]);
    assert(done[id_of(r3)] && !flag[2]);
    finish(r3);

    /* An access to a buffer only being read waits for its readers. */
    void* r4 = read_call(objects[3]);
    host_access(objects[3]);
    assert(done[id_of(r4)] && !flag[3]);
    /* An access to one being produced and read waits for both. */
    void* w4 = stub(writer, NULL, objects[4]);
    void* r5 = read_call(objects[4]);
    host_access(objects[4]);
    assert(done[id_of(w4)] && done[id_of(r5)] && !flag[4]);
    finish(r4);
    finish(r5);

    /* One task with the same buffer as input and output: the output mark
     * comes first, the read mark adds nothing, and nothing waits. */
    staged_arg* a = filc_async_alloc(2 * sizeof *a, 16);
    void* both = filc_async_begin(writer, a);
    filc_async_mark_pending(both, objects[5]);
    filc_async_mark_input(both, objects[5]);
    filc_async_submit(both, writer, NULL, a, 2);
    assert(flag[5]);
    finish(both);
    assert(!flag[5] && !filc_async_is_pending(objects[5]));
    /* The other order turns its read mark into a write mark. */
    a = filc_async_alloc(2 * sizeof *a, 16);
    void* upgrade = filc_async_begin(writer, a);
    filc_async_mark_input(upgrade, objects[5]);
    assert(flag[5] && !filc_async_is_pending(objects[5]));
    filc_async_mark_pending(upgrade, objects[5]);
    assert(flag[5] && filc_async_is_pending(objects[5]));
    filc_async_submit(upgrade, writer, NULL, a, 2);
    finish(upgrade);
    assert(!flag[5]);

    /* A call made from a body does not wait for that body's own read mark,
     * which would deadlock: the body is running on this thread. */
    void* outer = stub(nested, nested_body, objects[6]);
    assert(nested_inner && !done[id_of(outer)]);
    assert(flag[6]);
    finish(nested_inner);
    finish(outer);

    /* wait_all joins producers only: a buffer that is only read joins
     * nothing. */
    void* r6 = read_call(objects[7]);
    prov_tag tags[] = { objects[7] };
    void* token = filc_async_wait_all(tags, 1);
    assert(!filc_async_is_pending(token));
    void* w7 = write_call(token);
    assert(!done[id_of(r6)]);
    finish(w7);
    finish(r6);

    /* mark_resolved clears a producer's mark and leaves readers. */
    void* r7 = read_call(objects[8]);
    filc_async_mark_pending(NULL, objects[8]);
    assert(done[id_of(r7)] && flag[8]);
    filc_async_mark_resolved(objects[8]);
    assert(!flag[8] && !filc_async_is_pending(objects[8]));

    /* Many readers, then a writer: it waits for all of them. */
    enum { READERS = 200 };
    void* many[READERS];
    for (int i = 0; i < READERS; ++i)
        many[i] = read_call(objects[9]);
    assert(flag[9] && !filc_async_is_pending(objects[9]));
    for (int i = 0; i < READERS; ++i)
        assert(!done[id_of(many[i])]);
    void* last = write_call(objects[9]);
    for (int i = 0; i < READERS; ++i)
        assert(done[id_of(many[i])]);
    finish(last);
    assert(!flag[9]);

    /* A read-only object, which the program cannot store into, gets no flag
     * for its readers, so reading it never waits. */
    readonly[10] = 1;
    void* r10 = read_call(objects[10]);
    assert(!flag[10]);
    host_access(objects[10]);
    assert(!done[id_of(r10)]);
    /* A write mark still sets it. */
    void* w10 = write_call(objects[10]);
    assert(done[id_of(r10)] && flag[10]);
    finish(w10);
    assert(!flag[10]);

    /* A runtime reading a call's own input while it submits the call does
     * not wait for that call, which has not reached the runtime yet. */
    void* t11 = stub(toucher, NULL, objects[11]);
    assert(!done[id_of(t11)] && flag[11]);
    finish(t11);
    assert(!flag[11]);

    /* Nor does a body reading its own input. */
    void* t12 = stub(reader, touching_body, objects[12]);
    assert(!done[id_of(t12)]);
    finish(t12);

    /* A call that reads a buffer is a reader inside its body too: reading
     * its input there does not wait for the other calls reading it. */
    void* first_reader = read_call(objects[13]);
    void* second_reader = stub(reader, touching_body, objects[13]);
    assert(!done[id_of(first_reader)] && !done[id_of(second_reader)]);
    finish(first_reader);
    finish(second_reader);

    /* A writer whose body runs late, as on a thread pool, does not wait in
     * its body for a reader queued after it: that reader waits for the
     * writer, so waiting for it would be a deadlock. */
    void* producer = stub(late_writer, touching_body, objects[14]);
    void* consumer = read_call(objects[14]);
    unsigned consumer_id = id_of(consumer);
    finish(producer); /* runs the body */
    assert(!done[consumer_id]);
    finish(consumer);
    assert(!flag[14]);

    /* Calls made from a body do not wait for the bodies running on this
     * thread, at any depth: a nested call's body reads the outer call's
     * input, and a call made two levels down writes it. That writer still
     * waits for the separate call reading it. */
    void* outer_call = stub(nested, outer_body, objects[15]);
    assert(inner_reader && middle_call && inner_writer);
    assert(!done[id_of(outer_call)] && !done[id_of(middle_call)]);
    assert(done[id_of(inner_reader)]);
    finish(inner_reader);
    finish(inner_writer);
    finish(middle_call);
    finish(outer_call);

    /* Code built without the compiler's check waits the same way through
     * filc_async_wait_access (FASYNC_ACCESS): for readers too. */
    void* r16 = read_call(objects[15] + 32);
    filc_async_wait_access(objects[15]);
    assert(done[id_of(r16)]);
    finish(r16);

    filc_async_stats stats;
    filc_async_get_stats(&stats);
    assert(stats.tasks_submitted == stats.tasks_completed);
    puts("CHECK_WRITE_AFTER_READ PASS");
    return 0;
}
