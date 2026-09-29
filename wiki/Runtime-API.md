# Runtime API

**Header:** [`runtime/src/filc_async_runtime.h`](../runtime/src/filc_async_runtime.h)

This is the whole contract between the generic framework and a runtime. A
runtime implements three functions. The framework provides six services the
runtime may call. The framework never reads `op=` or argument shapes: which
operations exist, and how they run, is up to the runtime.

For a step-by-step guide, see [Write a runtime](Writing-a-Runtime.md). For a
complete example, see [the io_uring runtime](io_uring-Runtime.md).

- [Task lifecycle](#task-lifecycle)
- [Functions a runtime implements](#functions-a-runtime-implements):
  [`filc_async_submit`](#filc_async_submit),
  [`filc_async_runtime_poll`](#filc_async_runtime_poll),
  [`filc_async_runtime_validate`](#filc_async_runtime_validate)
- [Framework services](#framework-services):
  [`filc_async_complete`](#filc_async_complete),
  [`filc_async_run`](#filc_async_run),
  [`filc_async_task_runtime_data`](#filc_async_task_runtime_data),
  [`filc_async_resolve_buffer`](#filc_async_resolve_buffer),
  [`filc_async_mark_shared`](#filc_async_mark_shared),
  [`filc_async_wait_buffer`](#filc_async_wait_buffer)
- [Types](#types)
- [Staged arguments](#staged-arguments)
- [Rules for runtimes](#rules-for-runtimes)

## Task lifecycle

Every annotated call is one **task**, an opaque `void*`.

```text
stub:     filc_async_begin ─► lock deps ─► mark buffers ─► filc_async_submit ─► return task
runtime:                                                   (start the call)
                                                 ... later, on any thread ...
runtime:  filc_async_complete(task, result)   ─► buffers resolve, locks release
program:  filc_async_poll / filc_async_wait   ─► result delivered, handle retired
```

1. **The stub starts the task.** The pass-emitted stub creates it with
   `filc_async_begin`.
2. **Locks are taken.** One per `r_dep=` / `w_dep=` argument. The stub waits
   here for conflicting calls.
3. **Buffers are marked pending.** Every `bout=`, `buf=` and unannotated
   pointer argument. The stub waits here for calls that still own the buffer.
4. **The stub calls `filc_async_submit`.** When that returns, the stub returns
   the task to the caller.
5. **The runtime reports the outcome** with `filc_async_complete`, exactly
   once, from any thread, at any time: during submit, during a poll, or on its
   own thread.
6. **Completion.** The framework clears the task's pending marks, releases its
   locks, and wakes every thread waiting on it.

Until step 5, the framework asks the runtime about the task with
`filc_async_runtime_poll` whenever something needs the task finished: a
`filc_async_wait` or `filc_async_poll`, an access to one of its buffers, or a
later call that needs one of its locks or buffers.

## Functions a runtime implements

### filc_async_submit

```c
void filc_async_submit(void* task, const filc_async_meta* meta,
                       filc_async_run_fn run, void* staged_args, size_t nargs);
```

Starts `task`. By the time submit is called, the call has taken its
dependency locks and marked its buffers.

| Parameter | Description |
|---|---|
| `task` | The task handle. Pass it to the framework services. |
| `meta` | The function's descriptor: name, argument kinds, dependency bits, and the option strings in `meta->opts`. See [Framework API](Framework-API.md#descriptor). |
| `run` | The run thunk: calls the annotated function's body with the staged arguments. May be `NULL`. Call it only through `filc_async_run`. |
| `staged_args` | The call's arguments, `nargs` cells of 16 bytes each. See [Staged arguments](#staged-arguments). |
| `nargs` | Equal to `meta->nargs`. |

**Requirements**

- Arrange for `filc_async_complete(task, result)` to be called exactly once:
  before submit returns, or later from any thread.
- Keep `staged_args` and anything it points to reachable until the call
  completes. The framework keeps the array itself reachable from the task.
- An unsupported call is reported by completing it with a negative errno,
  such as `-EINVAL`. Do not abort.
- Called on the thread that made the annotated call, with no framework lock
  held.

### filc_async_runtime_poll

```c
bool filc_async_runtime_poll(void* task, enum filc_async_poll_mode mode);
```

Asks about `task`. Returns `true` once `filc_async_complete` has been called
for it.

| `mode` | Called when | The runtime may |
|---|---|---|
| `FILC_ASYNC_POLL_CHECK` | `filc_async_get_stats` refreshes its counts | only look: start nothing, never block |
| `FILC_ASYNC_POLL_PROGRESS` | the program calls `filc_async_poll` | start deferred work (flush a queue, run a body), but not block |
| `FILC_ASYNC_POLL_BLOCK` | something must wait for the task: `filc_async_wait`, an access to a pending buffer, a conflicting lock, re-marking an owned buffer | block until the task completes |

**Requirements**

- **Return value.** Return `true` only if `filc_async_complete` has been
  called for `task`, whether by this poll or earlier.
- **Tasks not yet submitted.** Poll can be called for a task whose submit has
  not run or not finished, for example from another thread waiting on its
  lock. Return `false`. For `BLOCK`, the framework then waits for a change
  and asks again, so returning `false` without blocking is allowed.
- **Reentrancy.** Poll can be reentered on the same thread. A body run inside
  a poll may touch another call's buffer, and that polls again. Use a
  recursive lock, or release your lock before running a body.
- **Threads.** Poll may be called from any thread, concurrently with submit
  and with other polls.
- **Tasks the runtime starts itself.** Poll is also called for these (see
  [`filc_async_task_runtime_data`](#filc_async_task_runtime_data)).

### filc_async_runtime_validate

```c
bool filc_async_runtime_validate(const filc_async_meta* meta);
```

Returns whether the runtime can run calls described by `meta`.

- **When it runs.** The pass-emitted constructor calls it once per annotated
  function in each translation unit, before `main`. That constructor has
  priority 65535, so it runs last.
- **On failure.** A `false` return aborts the program with
  `function cannot be registered on this runtime`.
- **What to check.** The op name in `meta->opts` and the argument kinds in
  `meta->args` your runtime needs.
- **Validator override.** If the program installed its own validator with
  `filc_async_set_validator`, that validator is used instead and this
  function is not called.

## Framework services

### filc_async_complete

```c
void filc_async_complete(void* task, long result);
```

Reports that `task` finished with `result`.

- **Result and state.** A negative `result` means failure: the task's state
  becomes 2 (failed) and `result` is conventionally `-errno`. Anything else
  sets the state to 0 (done). `filc_async_poll` and `filc_async_wait` deliver
  `result` to the program.
- **Effects.** Clears every buffer mark the task still holds, releases its
  dependency locks, updates the statistics, and wakes all waiters.
- **Calls after the first.** They are ignored.
- **Threads and locks.** Callable from any thread, including with the
  runtime's own locks held. It never calls back into the runtime.

### filc_async_run

```c
long filc_async_run(void* task, filc_async_run_fn run, void* staged_args);
```

Runs the body of `task` on the calling thread and returns its result. The
result is 0 when `run` is `NULL`.

- **Why not call `run` directly.** While the body runs, the framework knows
  which task it belongs to. The body can then read and write its own pending
  buffers without waiting for itself. Calling `run` directly would deadlock
  the first time the body touches its own output buffer.
- **Nesting.** Nested calls are fine: the previous running task is restored on
  return.
- **Locks.** Do not hold a runtime lock across this call. The body is user
  code and may wait for other calls.

### filc_async_task_runtime_data

```c
void** filc_async_task_runtime_data(void* task);
```

Returns the address of one word of per-task storage for the runtime.

- **Initial value.** `NULL`.
- **Reachability.** It is a Fil-C pointer slot, so whatever it points to stays
  reachable as long as the task does.
- **Publish last.** Store to it last in submit, after the runtime's state for
  the call is complete. A concurrent poll that reads `NULL` treats the task
  as not yet submitted.

**Tasks the runtime starts itself.** `filc_async_begin(NULL, NULL)` creates a
task without a stub, for work that did not come from an annotated call. The
io_uring runtime uses one for each explicit `fasync_pread`. Such a task:

- has no descriptor and no locks;
- is not counted in the statistics;
- is polled and completed like any other.

### filc_async_resolve_buffer

```c
void filc_async_resolve_buffer(void* task, void* buf);
```

Clears `task`'s mark on the object `buf` points into before the task
completes. Use it when part of a call's output is ready early. A buffer
`task` does not own is left alone.

### filc_async_mark_shared

```c
void filc_async_mark_shared(void* task, void* buf);
```

Marks the object `buf` points into pending for `task` without waiting for
its other owners. Several tasks can then own one object, and an access waits
for all of them.

- **Stubs versus runtimes.** A stub's marks are exclusive: they wait for
  earlier owners. This is for runtime-started tasks where overlapping
  ownership is intended, like several explicit reads into one array.
- **Duplicates.** Marking an object the task already owns does nothing.

### filc_async_wait_buffer

```c
void filc_async_wait_buffer(void* task, const void* buf);
```

Waits until no task other than `task` owns the object `buf` points into.
`task` may be `NULL`. Marks held by the task whose body is running on this
thread are also ignored.

Use it before handing a device or the kernel a buffer to **read**, when an
earlier call may still be filling it. An output buffer needs no such wait:
the stub already waited when it marked it.

## Types

```c
typedef long (*filc_async_run_fn)(void* staged_args);

enum filc_async_poll_mode {
    FILC_ASYNC_POLL_CHECK,
    FILC_ASYNC_POLL_PROGRESS,
    FILC_ASYNC_POLL_BLOCK
};
```

The run thunk (`__filc_async_run_<name>`, emitted by the pass) loads each
argument from its cell, calls the body, and converts the return value to a
`long`:

| Body returns | `run` returns |
|---|---|
| `void` | 0 |
| a pointer | its address as an integer |
| an integer | its value, sign-extended |

## Staged arguments

`staged_args` is an array of `nargs` cells, 16 bytes each. Fil-C pointers are
16 bytes: an address plus its capability.

| Argument type | Cell contents |
|---|---|
| pointer | the Fil-C pointer, capability included |
| integer ≤ 64 bits | the value zero-extended in the first 8 bytes; the second 8 bytes are 0 |
| anything else (floating point, aggregates, wider integers) | zero; the body receives 0 for it |

`sizeof(void*)` is 8 in Fil-C C source, so index the array with an explicit
16-byte struct, never with a plain pointer/word union:

```c
typedef struct {
    union { void* ptr; uint64_t word; } value;
    uint64_t capability;
} staged_arg;
_Static_assert(sizeof(staged_arg) == 16, "annotated argument slot must be 16 bytes");
```

An 8-byte stride reads the wrong cell. In the io_uring runtime, that made a
read of 6 bytes look larger than `UINT_MAX`.

## Rules for runtimes

- **Build with Fil-C.** Compile the runtime with Fil-C, and link it after the
  program and before the final `-lpizlo -lc`
  (see [Build and link](Building-and-Linking.md#link-a-program)).
- **Allocate with `filc_async_alloc`.** Use `filc_async_alloc(size, align)`
  (in `filc_async_alloc.h`) for per-call state. It returns zeroed memory, is
  thread-safe, and keeps memory reachable for the GC.
- **Lock ordering.** The framework never holds its lock while calling
  `filc_async_submit` or `filc_async_runtime_poll`. It is safe to call
  `filc_async_complete` with the runtime's lock held. Do not hold a runtime
  lock across `filc_async_run` or `filc_async_wait_buffer`, which may call
  back into the runtime.
- **Waiting.** A call waits in its stub only through the framework, which
  polls the call it waits for. A runtime that parks calls must make progress
  when polled with `BLOCK`. Otherwise a program that only touches buffers
  never finishes.
- **Failure.** Use `filc_async_fatal(msg)` only for unrecoverable internal
  errors. Report per-call failures through `filc_async_complete` with a
  negative result.
