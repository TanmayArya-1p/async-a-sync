# The io_uring runtime

**Source:** [`runtime/src/filc_async_uring.c`](../runtime/src/filc_async_uring.c),
on top of the request layer in `runtime/src/fasync*.c`
**Library:** `runtime/build/lib/libfilc_async_uring.a`

This is the runtime shipped with the repository, `runtime=io_uring`. It turns
annotated calls into io_uring requests. It is also a worked example of the
[Runtime API](Runtime-API.md): this page covers what it supports and then walks
through how it implements each runtime function.

## Supported operations

| `op=` | Required signature (kinds) | io_uring request | Result |
|---|---|---|---|
| `pread` | `(fd, bout= or buf=, len, offset)` | `IORING_OP_READ` | bytes read or `-errno` |
| `pwrite` | `(fd, bin= or buf=, len, offset)` | `IORING_OP_WRITE` | bytes written or `-errno` |
| `openat` | `(dirfd, bin= or buf= path, flags, mode)` | `IORING_OP_OPENAT` | the new fd or `-errno` |
| `fsync` | `(fd)` | `IORING_OP_FSYNC` | 0 or `-errno` |
| `close` | `(fd)` | `IORING_OP_CLOSE` | 0 or `-errno` |
| `ignore` | anything | none | `-EOPNOTSUPP` when polled; for tests only |

- **Arguments.** They follow the syscall's order. The descriptor comes first,
  and it, `len` and `offset` must be unannotated integers: the runtime finds
  the descriptor by its position. A `len` above `UINT_MAX` completes with `-EOVERFLOW`.
  `read` and `write` are not supported because they have no offset.
- **Example declaration:**

  ```c
  #pragma clang attribute push(__attribute__((annotate("filc_async", "runtime=io_uring", "op=pwrite", "bin=1", "w_dep=0"))), apply_to=function)
  void* async_pwrite(int fd, const void* buf, size_t len, unsigned long offset);
  #pragma clang attribute pop
  ```

- **Capacity.** At most 1024 requests are in flight (`FASYNC_MAX_INFLIGHT`).
  All threads share one ring.

## How it works

The runtime's descriptor is the only symbol programs refer to. It is declared
in `fasync.h`, and its three functions are `static`:

```c
FILC_ASYNC_RUNTIME(io_uring, uring_submit, uring_poll, uring_validate);
```

A program that names `runtime=io_uring` links `-lfilc_async_uring`, which
may sit next to other runtimes.

```text
uring_submit(task, meta, run, args, nargs)
  ├─ filc_async_run(task, run, args)           run the body (no lock held)
  ├─ filc_async_wait_buffer(task, args[1])     pwrite/openat: wait for the source
  └─ fasync_lock()
       queue(): validate shape → fasync_do_<op>() → SQE queued, not yet sent
     fasync_unlock()

uring_poll(task, mode)
  └─ fasync_lock()
       CHECK:    is the CQE there?
       PROGRESS: send queued SQEs to the kernel, then check
       BLOCK:    send queued SQEs, wait for this request
       ready → finish(): collect the result, free the slot, filc_async_complete
     fasync_unlock()
```

The central idea is **deferred submission**:

- **Submit only queues.** It writes an SQE into the ring but does not enter
  the kernel.
- **The first wait sends the batch.** The first thing that needs any result
  (an access, a wait, a lock conflict) polls with `PROGRESS` or `BLOCK`, and
  that sends every queued SQE in one `io_uring_enter`.
- **The effect.** A loop of N annotated reads followed by a loop that touches
  the buffers costs one kernel submission, not N.

### Per-call state

```c
struct uring_call {
    void* task;
    fasync_id request;         /* slot in the request table; 0 for op=ignore */
    enum uring_op op;
    bool explicit_read;        /* a fasync_pread; its caller collects the result */
    bool finished;             /* completion reported */
    struct uring_call* newer;  /* calls with a request in flight, oldest first */
    struct uring_call* older;
};
```

One `uring_call` per task, allocated with `filc_async_alloc` and stored in the
task's runtime-data slot. Calls with a request in flight are kept on a list,
oldest first, for [reclaiming slots](#reclaiming-request-slots).

### Validation

`uring_validate` parses `op=` out of `meta->opts` (`op_from`). It
then checks the argument kinds against the op's syscall (`shape_ok`):

```c
case URING_OP_READ:
    return m->nargs == 4 && is_output_kind(m->args[1].kind) &&
           m->args[2].kind == FILC_ASYNC_ARG_IGNORED &&
           m->args[3].kind == FILC_ASYNC_ARG_IGNORED;
```

Argument 0, the descriptor, must be an unannotated integer. An unknown op or
a wrong shape aborts the program before `main`.

### Submit

```c
static void uring_submit(void* task, const filc_async_meta* meta,
                         filc_async_run_fn run, void* staged_args, size_t nargs)
{
    filc_async_run(task, run, staged_args);          // 1. the body
    struct uring_call* c = filc_async_alloc(sizeof *c, 16);
    c->task = task;
    c->op = op_from(meta->opts);
    const staged_arg* args = staged_args;
    if ((c->op == URING_OP_WRITE || c->op == URING_OP_OPENAT) && shape_ok(meta, c->op))
        filc_async_wait_buffer(task, arg_ptr(args, 1)); // 2. the kernel will read it
    fasync_lock();
    queue(c, meta, args);                            // 3. the SQE
    fasync_unlock();
}
```

1. **The body runs first**, with no lock held, so a program can log or
   instrument each call. Its return value is ignored: the call's result is
   the kernel's.
2. **Input buffers are waited for.** For `pwrite` and `openat`, the kernel
   reads argument 1 when the request runs. If an earlier call is still
   filling that buffer, submit waits for it through the framework. The
   `pread` output buffer needs no wait: the stub already waited when it marked
   it.
3. **The request is queued** under the runtime's lock. `queue` checks the
   shape again (a program may have installed a permissive validator) and
   calls the request layer (`fasync_do_pread`, …). It links the call into the
   in-flight list, then publishes it in the runtime-data slot **last**, so a
   poll from another thread never sees a call without its request. A
   failure completes the task with `-errno` at once.

### Poll

```c
static bool uring_poll(void* task, enum filc_async_poll_mode mode)
{
    fasync_lock();
    struct uring_call* c = *filc_async_task_runtime_data(task);
    bool done = false;
    if (!c) {
        /* not submitted yet, or completed at submission */
    } else if (c->finished) {
        done = true;
    } else {
        bool ready = fasync_ready(c->request);         // CQE already reaped?
        if (!ready && mode != FILC_ASYNC_POLL_CHECK) {
            fasync_submit();                            // send the queued batch
            ready = mode == FILC_ASYNC_POLL_BLOCK || fasync_ready(c->request);
        }
        if (ready) { finish(c); done = true; }
    }
    fasync_unlock();
    return done;
}
```

- **Modes.** `CHECK` only reads the completion queue. `PROGRESS` also sends
  the queue. `BLOCK` makes `finish` wait for the request's completion.
- **Slot `NULL`.** Either the task has not been submitted, or it failed at
  submission and was completed then. In both cases, return `false` and let
  the framework read the task's state.
- **`finish`.** Reads the request's result, which frees its table slot,
  removes the call from the in-flight list, and calls
  `filc_async_complete(task, result)`.

### Reclaiming request slots

A program that only touches buffers never polls explicitly, so nothing would
free request slots. When the table is full, the request layer fails with
`EAGAIN`. `queue` then calls `reclaim`:

- walk the in-flight list from the oldest call, finishing every call whose
  request has completed;
- stop at the first call still running;
- if no call could be finished, send the queue and wait for the oldest.

The new request then takes a freed slot. `tests/t_pragma_lazy_many.c` issues
more calls than the table holds.

### Locking

- **One recursive lock (`fasync_lock`)** guards the ring, the request table
  and the pending-fd table. It is recursive because a body run during a poll
  can touch another call's buffer, and that polls again on the same thread.
- **Never held across framework waits.** The lock is not held while
  `filc_async_run` or `filc_async_wait_buffer` runs, since both may wait for
  other calls. Calling `filc_async_complete` with it held is allowed.
- **Safety, not parallelism.** Every thread's requests go through one ring.

### Explicit reads through the framework

The hand-written [explicit API](Explicit-API.md) uses the same request layer.
`fasync_pread` keeps lazy resolution by giving each read a task of its own:

```c
c->task = filc_async_task_new(&filc_async_runtime_io_uring); // runtime-owned task
*filc_async_task_runtime_data(c->task) = c;
filc_async_mark_shared(c->task, buf);          // several reads may share one buffer
```

- **Waiting.** An access to the buffer waits for the read through the
  framework, exactly like an annotated call.
- **Shared marks.** The marks are shared, not exclusive, so several explicit
  reads can target one array without waiting for each other.
- **Result.** An explicit read's result stays in its request slot for the
  caller's `fasync_result`, which also completes the task.

## Files

In `libfilc_async_uring.a`, built with Fil-C:

| File | Role |
|---|---|
| `filc_async_uring.c` | the runtime functions: validation, submit, poll, reclaim, explicit read tracking |
| `fasync.c` | ring setup (GC-pinned, page-aligned memory, `IORING_SETUP_NO_MMAP`), SQE queueing, submission, the runtime lock |
| `fasync_syscalls.c` | the `fasync_*` operations, bounds checks on buffers and paths, pending fds for explicit opens |
| `fasync_token.c`, `fasync_dep.c` | the explicit API's provenance tokens and effect-set dependency DAG (unrelated to `r_dep=`/`w_dep=`) |

In `libpizlo.a`, built with the host compiler against Fil-C's runtime:

| File | Role |
|---|---|
| `fasync_native.c` | the io_uring syscalls: checks Fil-C capabilities before handing raw addresses to the kernel |
