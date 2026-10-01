# Write a runtime

This guide shows how to put your own runtime behind annotated calls, next to
or instead of io_uring. A runtime decides what a call does and when it
completes: run the body inline, run it lazily, hand it to a thread pool, or
turn it into a device request. Each annotated function picks its runtime with
`runtime=<name>`, so one program can use several.

You will:

1. write the smallest runtime that works;
2. make it lazy, so bodies run only when something needs their results;
3. read your own options from the annotation;
4. link and test it.

Every function used here is specified in the [Runtime API](Runtime-API.md)
reference.

## 1. The smallest runtime

A runtime is three functions and a descriptor that names them. This one, the
`mock` runtime, runs every call's body synchronously during submit and
completes the call right away. It is
[`tests/support/mock_runtime.c`](../tests/support/mock_runtime.c):

```c
#include "filc_async_runtime.h"

static void mock_submit(void* task, const filc_async_meta* meta,
                        filc_async_run_fn run, void* staged_args, size_t nargs)
{
    (void)meta;
    (void)nargs;
    filc_async_complete(task, filc_async_run(task, run, staged_args));
}

static bool mock_poll(void* task, enum filc_async_poll_mode mode)
{
    (void)task;
    (void)mode;
    return true;
}

static bool mock_validate(const filc_async_meta* meta)
{
    (void)meta;
    return true;
}

FILC_ASYNC_RUNTIME(mock, mock_submit, mock_poll, mock_validate);
```

`FILC_ASYNC_RUNTIME` defines `filc_async_runtime_mock`, the descriptor that
functions annotated with `runtime=mock` point to:

```c
FILC_ASYNC(mock, FILC_OP(double), FILC_BOUT(out), FILC_BIN(in))
void* double_into(long* out, const long* in);
```

- **The descriptor is the only export.** The functions are `static`, so this
  runtime links next to any other without clashing.

- **Submit.** It runs the body through `filc_async_run`, never by calling
  `run` directly. The body can then write its own `bout=` buffer, which the
  stub has already marked pending, without waiting for itself.
- **Poll.** It returns `true` because every task completes during its submit.
  Strictly, a task polled from another thread before its submit has run is
  not complete yet. The framework tolerates the early `true` by checking the
  task's own state again, at the cost of spinning until submit finishes. The
  next runtime handles this case properly.
- **Validate.** It accepts every function.

## 2. A lazy runtime

This runtime does no work at submit. A body runs the first time anyone
polls its task with `PROGRESS` or `BLOCK`. That happens on the first access
to its output buffer, on `filc_async_wait`, or when a later call needs one of
its locks.

```c
#include <errno.h>
#include <pthread.h>

#include "filc_async_alloc.h"
#include "filc_async_runtime.h"

enum { QUEUED, RUNNING, DONE };

struct lazy_call {
    filc_async_run_fn run;
    void* staged_args;
    int state;
};

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_done = PTHREAD_COND_INITIALIZER;

static void lazy_submit(void* task, const filc_async_meta* meta,
                        filc_async_run_fn run, void* staged_args, size_t nargs)
{
    (void)meta;
    (void)nargs;
    struct lazy_call* c = filc_async_alloc(sizeof *c, 16);
    if (!c) {
        filc_async_complete(task, -ENOMEM);
        return;
    }
    c->run = run;
    c->staged_args = staged_args;
    c->state = QUEUED;
    pthread_mutex_lock(&g_lock);
    *filc_async_task_runtime_data(task) = c;   // publish last
    pthread_mutex_unlock(&g_lock);
}

static bool lazy_poll(void* task, enum filc_async_poll_mode mode)
{
    pthread_mutex_lock(&g_lock);
    struct lazy_call* c = *filc_async_task_runtime_data(task);
    if (!c) {
        // not submitted yet: the framework waits and asks again
        pthread_mutex_unlock(&g_lock);
        return false;
    }
    if (c->state == QUEUED && mode != FILC_ASYNC_POLL_CHECK) {
        c->state = RUNNING;
        // never hold a lock across the body: it may wait for other calls
        pthread_mutex_unlock(&g_lock);
        long result = filc_async_run(task, c->run, c->staged_args);
        filc_async_complete(task, result);
        pthread_mutex_lock(&g_lock);
        c->state = DONE;
        pthread_cond_broadcast(&g_done);
    }
    while (c->state == RUNNING && mode == FILC_ASYNC_POLL_BLOCK)
        pthread_cond_wait(&g_done, &g_lock);
    bool done = c->state == DONE;
    pthread_mutex_unlock(&g_lock);
    return done;
}

static bool lazy_validate(const filc_async_meta* meta)
{
    return meta != NULL;
}

FILC_ASYNC_RUNTIME(lazy, lazy_submit, lazy_poll, lazy_validate);
```

This runtime follows every rule a real one needs:

- **Store per-call state in the task's runtime-data slot.** Allocate it with
  `filc_async_alloc`, so it is reachable for as long as the task is.
- **Publish the slot last.** A poll that sees `NULL` means "not submitted
  yet", and returns `false`.
- **Respect the poll mode.** `CHECK` never runs anything. `PROGRESS` may run
  the body. `BLOCK` waits if another thread is already running it.
- **Hold no lock across `filc_async_run`.** The body is user code. It may
  touch another call's buffer, which polls again, on this thread or on
  another.
- **Complete exactly once.** The `RUNNING` state keeps two threads from
  running one body.

With a function annotated `runtime=lazy`, a call's buffer stays pending after
the call returns, and the first read of it runs the body:

```c
double_into(&out, &in);                // runtime=lazy: queued; nothing has run
assert(filc_async_is_pending(&out));
long v = out;                          // this access runs double_into's body
```

A thread-pool runtime has the same shape. Submit pushes `(task, run,
staged_args)` onto a queue. A worker calls `filc_async_run` and then
`filc_async_complete`. Poll with `BLOCK` waits on a condition variable until
the worker is done.

## 3. Read your own options

Every annotation string the pass does not interpret itself is passed through
in `meta->opts`, a `NULL`-terminated array. That includes `op=` and anything
you invent, such as `"prio=high"`. Parse the ones your runtime needs, and
reject unknown functions at startup in your `validate`:

```c
static const char* option(const filc_async_meta* meta, const char* key)
{
    size_t n = strlen(key);
    for (size_t i = 0; meta->opts && meta->opts[i]; ++i)
        if (!strncmp(meta->opts[i], key, n) && meta->opts[i][n] == '=')
            return meta->opts[i] + n + 1;
    return NULL;
}

static bool my_validate(const filc_async_meta* meta)
{
    const char* op = option(meta, "op");
    return op && (!strcmp(op, "compute") || !strcmp(op, "hash"));
}
```

Check argument shapes here too. `meta->args[i].kind` says what each argument
was annotated as (`FILC_ASYNC_ARG_BUFFER_IN`, `_BUFFER_OUT`, `_PENDING`,
`_IGNORED`). Calls to functions that fail validation never
start: the program aborts before `main`. The io_uring runtime's
[`shape_ok`](io_uring-Runtime.md#validation) is an example.

## 4. Use the arguments

If your runtime does the work itself instead of running the body, read the
arguments from `staged_args`. Each argument occupies a 16-byte cell (see
[Staged arguments](Runtime-API.md#staged-arguments)):

```c
typedef struct {
    union { void* ptr; uint64_t word; } value;
    uint64_t capability;
} staged_arg;

const staged_arg* args = staged_args;
int fd = (int)args[0].value.word;
void* buf = args[1].value.ptr;          // a real Fil-C pointer, bounds included
```

Before giving a device a buffer to **read** (a `bin=` argument), call
`filc_async_wait_buffer(task, buf)`: an earlier call may still be filling it.
If part of the output is ready before the whole call, release it early with
`filc_async_resolve_buffer(task, buf)`.

## 5. Link and test

Link your runtime's objects or archive before `-lpizlo`, next to every other
runtime the program's annotations name:

```sh
# only your runtime
vendor/fil-c-src/build/bin/filcc -O2 -static \
  -Iruntime/include -Lruntime/build/lib \
  -o app app.c my_runtime.c -lpizlo -lc

# your runtime and the io_uring runtime in one program
vendor/fil-c-src/build/bin/filcc -O2 -static \
  -Iruntime/include -Lruntime/build/lib \
  -o app app.c my_runtime.c -lfilc_async_uring -lpizlo -lc
```

A runtime refers to the framework, but the framework never refers to a
runtime, so runtimes go before `-lpizlo`. If a function names a runtime you
did not link, the link fails with
`undefined reference to pizlonated_filc_async_runtime_<name>`.

To test it, run the annotated programs in `tests/` against it. Check these
invariants:

- **Buffers stay pending.** `filc_async_is_pending(buf)` is true after the
  call and false after the access or the wait.
- **Waits return results.** `filc_async_wait` returns the body's return value,
  or your runtime's result.
- **Statistics add up.** `filc_async_get_stats` reports
  `tasks_submitted == tasks_completed` once every call is waited on.
- **Dependencies hold.** Two calls with a `w_dep=` on the same value run in
  call order: the second call's stub waits, polling the first with `BLOCK`.
- **Threads work.** Calls from several threads touching each other's buffers
  finish. `tests/io_uring/t_threads.c` exercises this.

`tests/framework/t_mock_runtime.c` shows the shape of such a test, and `tests/run.sh`
builds it with `tests/support/mock_runtime.c` and checks that no io_uring symbol was
linked. `tests/framework/t_two_runtimes.c` links the mock runtime with the io_uring
runtime and orders calls across them. `demos/rpc/demo_rpc_upload.c` does the
same with a runtime that talks to a server: its uploads wait for io_uring
reads (`make demo-rpc-upload`).

## See also

- [Runtime API](Runtime-API.md): the full contract.
- [The io_uring runtime](io_uring-Runtime.md): a runtime that turns calls into
  kernel requests, batches them, and reclaims request slots.
- [The RPC runtime example](RPC-Runtime.md): a runtime that sends calls to a
  TCP server and returns its replies as results.
- [Architecture](Architecture.md): where the stub, framework and runtime meet.
