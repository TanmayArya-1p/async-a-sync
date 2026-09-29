# Architecture

This page explains how an annotated call becomes asynchronous, and why the
pieces are split the way they are. The interfaces themselves are specified in
the [Runtime API](Runtime-API.md) and [Framework API](Framework-API.md).

## Three layers

```text
 ┌────────────── compiler (patched Fil-C clang) ──────────────┐
 │ FilAsync pass:     descriptor, stub, run thunk per function │
 │ FilPizlonator:     pending-flag test before pointer access  │
 └──────────────────────────────┬──────────────────────────────┘
                                │ filc_async.h
 ┌──────────────── framework (libpizlo.a) ─────────────────────┐
 │ tasks, dependency locks, pending marks, header flag, waits  │
 │ knows nothing about ops or io_uring                         │
 └──────────────────────────────┬──────────────────────────────┘
                                │ filc_async_runtime.h
 ┌──────────────── runtime (e.g. libfilc_async_uring.a) ───────┐
 │ decides what a call does and when it completes              │
 └─────────────────────────────────────────────────────────────┘
```

- **The compiler** turns each call into a call to a stub, and makes every
  pointer access check whether its object is pending.
- **The framework** tracks who owns what. It holds tasks, buffer ownership
  (marks), and dependency locks, and it waits by polling the runtime.
- **The runtime** executes calls. The shipped runtime turns them into io_uring
  requests. Any runtime that implements the three functions of
  `filc_async_runtime.h` can replace it. `tests/mock_runtime.c` does.

The framework references no symbol of any runtime except those three
functions. `tests/check_linkage.sh` and the mock-runtime test enforce that.

## One call, end to end

```text
read_at(fd, buf, len, 0)
  └─► __filc_async_stub_read_at                          (emitted by FilAsync)
        stage args into 16-byte cells
        task = filc_async_begin(meta, staged)
        filc_async_lock_word(task, fd, ns, READ)         one per r_dep=/w_dep=
        filc_async_mark_pending(task, buf)               one per output buffer
        filc_async_submit(task, meta, run, staged, 4)    ─► runtime
        return task
...
c = buf[0]
  └─► if (header(buf).aux & PENDING)                    (emitted by FilPizlonator)
        filc_resolve_pending(buf, lower)                 ─► framework
          wait for every task owning the object
            filc_async_runtime_poll(task, BLOCK)         ─► runtime
              ... filc_async_complete(task, result)      ◄─ runtime
          marks cleared, flag cleared, locks released
      load buf[0]
```

## Compiler

### FilAsync pass

`compiler/upstream-overrides/llvm/lib/Transforms/Instrumentation/FilAsync.cpp`

**Where it runs.** Clang records the pragma in `llvm.global.annotations`.
`BackendUtil.cpp` installs FilAsync at the very start of Fil-C's pipeline,
before any optimization and before FilPizlonator. Keep that order: the
inliner and attribute inference must never see a direct call to an annotated
function, or they could fold or inline it before it is redirected
(`tests/t_pragma_same_tu_lazy.c`).

**What it emits.** For each annotated function `F`, the pass emits:

- `__filc_meta_F`: the descriptor (`filc_async_meta`) with argument kinds,
  dependency bits and a copy of every option string;
- `__filc_async_stub_F`: the stub that every direct call is redirected to;
- `__filc_async_run_F`: the run thunk that unpacks the staged cells and calls
  `F`'s body;
- a per-translation-unit table of descriptors, and a constructor at priority
  65535 that validates them before `main`.

**Linkage.** An annotated declaration keeps its ordinary linker name, so its
definition can live in another translation unit. A definition in the same
unit is renamed `__filc_async_F`. A non-static one keeps `F` as an alias, so
other units still link (`tests/t_linked_async_*.c`).

**What it leaves alone.** The pass only validates positional indices and
dependency types. It never checks `op=`: the runtime decides which ops exist.

### The access hook

`compiler/upstream-overrides/llvm/lib/Transforms/Instrumentation/FilPizlonator.cpp`

FilPizlonator is Fil-C's pass that turns pointers into capabilities. Before
the capability check on an access through an escaping pointer, the patched
version inserts:

```text
if (lower != 0 && (object_header(lower)->aux & FILC_OBJECT_FLAG_ASYNC_PENDING))
    filc_resolve_pending(ptr, lower);
```

**Cost.** The `aux` word sits next to the `upper` bound the capability check
loads anyway, so the fast path is one load and one branch on a warm cache
line.

**Where the slow path goes.** `filc_resolve_pending` (native, in
`filc_async_native.c`) calls the framework's resolver, which waits for the
owners.

**The address does not change.** The kernel or runtime writes into the
original buffer, so the access proceeds on the same pointer.

## Framework

`runtime/src/filc_async.c`, `filc_async_native.c`, `filc_async_arena.c`

**Tasks.** One per call. Running tasks are kept in a list, oldest first, and
tasks whose result has not been delivered are kept in a hash table of
handles.

**Pending marks.** A mark covers a whole Fil-C object,
`[zgetlower, zgetupper)`. Live objects never overlap, so marks are hashed by
the object's lower bound.

- **The header flag.** While an object has at least one mark, the framework
  sets `FILC_OBJECT_FLAG_ASYNC_PENDING` (flag value 64, the one flag bit Fil-C
  leaves free) in the object header's `aux` word. It uses the same
  compare-and-swap loop Fil-C uses for its own flags.
- **Why a flag.** Unmarked objects never reach the framework: the inline test
  fails and the access proceeds.
- **Exclusive marks.** Marks made by a stub are exclusive. Marking an object
  another call owns first waits for that call, so two calls never write one
  buffer at once.
- **Shared marks.** Marks made by a runtime with `filc_async_mark_shared` can
  overlap.
- **Self-access.** A body running under `filc_async_run` ignores its own
  task's marks.

**Dependency locks.**

- **Keys.** One lock per (key, namespace), where the key is an integer value
  or an object base.
- **Modes.** Readers share and writers exclude.
- **Order.** Requests queue in arrival order, so a stream of readers cannot
  starve a waiting writer.
- **Where calls wait.** A call that must wait does so in its stub, before
  reaching the runtime, by polling the holder.
- **Why locks and not a graph.** The runtime sees calls only in an order that
  is already safe, so no runtime needs to understand dependencies.

**Waiting.**

- **The only way to wait.** Every wait (for a lock holder, a buffer's owner,
  or `filc_async_wait`) is a loop of `filc_async_runtime_poll(task, BLOCK)`.
- **Progress.** The framework never assumes the runtime makes progress on its
  own. A runtime that queues work, like io_uring, gets its chance to flush
  whenever someone needs a result.
- **Unsubmitted tasks.** If the task is not yet submitted, the framework waits
  on a condition variable instead of polling.

**Memory.** Tasks, marks, locks and staged arguments come from
`filc_async_alloc`, a GC-backed arena by default, so everything the kernel may
still reference stays reachable.

## Runtime

A runtime implements submit, poll and validate, and reports results with
`filc_async_complete`. Because the framework already did the locking and
marking, a runtime can be very small (see [Write a runtime](Writing-a-Runtime.md)).

The io_uring runtime:

- runs the body;
- waits for any input buffer still being produced;
- queues an SQE;
- sends queued SQEs to the kernel only when a poll needs a result.

A loop of calls therefore reaches the kernel in one batch. See
[The io_uring runtime](io_uring-Runtime.md).

## Threading

**Framework.** Any thread may make annotated calls, wait on them, and touch
their buffers.

- **One lock.** The framework guards all its state with one mutex and
  broadcasts a condition variable when a task completes or a lock changes
  hands.
- **Lock ordering.** It never holds that mutex while calling into the runtime,
  so a runtime may complete tasks from any thread while holding its own
  locks.
- **Arena.** The arena has its own lock.
- **Compute-only threads.** A thread that only computes on its own memory
  never finds a pending flag, so it never calls into the framework
  (`tests/t_thread_compute.c`).

**io_uring runtime.** It serializes on one recursive lock around a single
ring. That gives safety, not I/O parallelism across threads.

## Source map

| Area | Files |
|---|---|
| Call rewriting, descriptors, stubs, run thunks | `compiler/upstream-overrides/llvm/lib/Transforms/Instrumentation/FilAsync.cpp` |
| Pending-flag test at access sites | `compiler/upstream-overrides/llvm/lib/Transforms/Instrumentation/FilPizlonator.cpp` |
| Pass order | `compiler/upstream-overrides/clang/lib/CodeGen/BackendUtil.cpp` |
| Framework | `runtime/src/filc_async.c`, `filc_async.h`, `filc_async_runtime.h` |
| Header flag and resolver bridge | `runtime/src/filc_async_native.c` |
| Allocator | `runtime/src/filc_async_arena.c`, `filc_async_alloc.h` |
| io_uring runtime | `runtime/src/filc_async_uring.c`, `fasync*.c` |
| Native forwarder generator | `runtime/upstream-overrides/generate_pizlonated_forwarders.rb` |
