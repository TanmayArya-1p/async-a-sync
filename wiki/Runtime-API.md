# Runtime API

**Header:** [`runtime/include/filc_async_runtime.h`](../runtime/include/filc_async_runtime.h)

This is the whole contract between the generic framework and a runtime.

- **What a runtime provides.** A **descriptor** holding three functions:
  submit, poll and validate.
- **What the framework provides.** Seven services the runtime may call.
- **What the framework ignores.** It never reads `op=` or argument shapes.
  Which operations exist, and how they run, is up to the runtime.

Each annotated function names its runtime with `runtime=<name>`. One program
can use several runtimes: it links each one its annotations name, and every
call goes to the runtime of its own function.

For a step-by-step guide, see [Write a runtime](Writing-a-Runtime.md). For a
complete example, see [the io_uring runtime](io_uring-Runtime.md).

- [Task lifecycle](#task-lifecycle)
- [The runtime descriptor](#the-runtime-descriptor):
  [`submit`](#submit), [`poll`](#poll), [`validate`](#validate)
- [Framework services](#framework-services):
  [`filc_async_complete`](#filc_async_complete),
  [`filc_async_run`](#filc_async_run),
  [`filc_async_task_runtime_data`](#filc_async_task_runtime_data),
  [`filc_async_task_new`](#filc_async_task_new),
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
runtime:                                                   rt->submit: start the call
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
4. **The stub calls `filc_async_submit`.** The framework passes the call to
   the `submit` of the runtime the function names (`meta->runtime`). When that
   returns, the stub returns the task to the caller.
5. **The runtime reports the outcome** with `filc_async_complete`, exactly
   once, from any thread, at any time: during submit, during a poll, or on its
   own thread.
6. **Completion.** The framework clears the task's pending marks, releases its
   locks, and wakes every thread waiting on it.

Until step 5, the framework asks the task's runtime about it with that
runtime's `poll` whenever something needs the task finished: a
`filc_async_wait` or `filc_async_poll`, an access to one of its buffers, or a
later call that needs one of its locks or buffers.

## The runtime descriptor

```c
typedef struct filc_async_runtime {
    const char* name;
    void (*submit)(void* task, const filc_async_meta* meta,
                   filc_async_run_fn run, void* staged_args, size_t nargs);
    bool (*poll)(void* task, enum filc_async_poll_mode mode);
    bool (*validate)(const filc_async_meta* meta);
} filc_async_runtime;

#define FILC_ASYNC_RUNTIME(name, submit, poll, validate) \
    const filc_async_runtime filc_async_runtime_##name = { #name, submit, poll, validate }
```

**Defining it.** A runtime named `foo` defines its descriptor as a global
called `filc_async_runtime_foo`, normally with the macro, and keeps its three
functions `static`:

```c
FILC_ASYNC_RUNTIME(foo, foo_submit, foo_poll, foo_validate);
```

**How calls find it.** A function annotated `runtime=foo` has
`meta->runtime == &filc_async_runtime_foo`. The pass emits that reference, so
a program that names `foo` without linking it fails to link with
`undefined reference to pizlonated_filc_async_runtime_foo`.

**Several runtimes.** The names only have to differ. The runtimes share the
framework's tasks, locks and pending buffers:

- a call waiting on a call of another runtime polls it through that
  runtime's `poll`;
- dependency keys and buffers order calls across runtimes, exactly as within
  one.

### submit

```c
void (*submit)(void* task, const filc_async_meta* meta,
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
- Called through the framework's `filc_async_submit`, on the thread that
  made the annotated call, with no framework lock held.

### poll

```c
bool (*poll)(void* task, enum filc_async_poll_mode mode);
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
  [`filc_async_task_new`](#filc_async_task_new)).

### validate

```c
bool (*validate)(const filc_async_meta* meta);
```

Returns whether the runtime can run calls described by `meta`.

- **When it runs.** The pass-emitted constructor calls it once for each
  annotated function that names this runtime, in each translation unit,
  before `main`. That constructor has
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

### filc_async_task_new

```c
void* filc_async_task_new(const filc_async_runtime* rt);
```

Creates a task of runtime `rt` without a stub, for work that did not come from
an annotated call. The io_uring runtime uses one for each explicit
`fasync_pread`. Such a task:

- has no descriptor and no locks;
- is not counted in the statistics;
- is polled through `rt->poll` and completed like any other.

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
typedef long (*filc_async_run_fn)(void* staged_args);   /* in filc_async.h */

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
- **Keep private symbols private.** Make the runtime's functions and state
  `static`, and export only the descriptor (and any API of your own), so
  several runtimes link into one program without clashing.
- **Allocate with `filc_async_alloc`.** Use `filc_async_alloc(size, align)`
  (in `filc_async_alloc.h`) for per-call state. It returns zeroed memory, is
  thread-safe, and keeps memory reachable for the GC.
- **Lock ordering.** The framework never holds its lock while calling a
  runtime's `submit` or `poll`. It is safe to call
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
