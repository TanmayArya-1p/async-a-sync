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
                                │ filc_async_runtime.h: one descriptor per runtime
 ┌─── runtime=io_uring ────────┐  ┌─── runtime=<other> ─────────┐
 │ libfilc_async_uring.a       │  │ another linked runtime      │
 │ io_uring requests           │  │ e.g. a thread pool          │
 └─────────────────────────────┘  └─────────────────────────────┘
```

- **The compiler** turns each call into a call to a stub, and makes every
  pointer access check whether its object is pending.
- **The framework** tracks who owns what. It holds tasks, buffer ownership
  (marks), and dependency locks, and it waits by polling the runtime.
- **Runtimes** execute calls.
  - **Choosing one.** Each annotated function names its runtime with
    `runtime=<name>`, and the program links every runtime it names.
  - **What a runtime is.** A `filc_async_runtime` descriptor with submit, poll
    and validate functions, exported as `filc_async_runtime_<name>`.
  - **The shipped runtimes.** `runtime=io_uring` (`runtime/io_uring/`) turns
    calls into io_uring requests. `runtime=rpc` (`runtime/rpc/`) sends calls
    to the rpc demos' TCP server, and `demos/rpc/demo_rpc_upload.c` reads
    files with io_uring and uploads them with it
    ([example](RPC-Runtime.md)). Each is a library of its own.
  - **A program's own runtime.** `tests/support/mock_runtime.c`, `runtime=mock`, is
    compiled into the tests that use it, and `tests/framework/t_two_runtimes.c` uses it
    next to io_uring.

**No runtime symbols in the framework.** The framework references no symbol
of any runtime. It reaches a task's runtime only through the descriptor its
function's `filc_async_meta` points to. `tests/io_uring/check_linkage.sh` enforces
this.

## One call, end to end

```text
read_at(fd, buf, len, 0)
  └─► __filc_async_stub_read_at                          (emitted by FilAsync)
        stage args into 16-byte cells
        task = filc_async_begin(meta, staged)
        filc_async_lock_word(task, fd, space, READ)      one per r_dep=/w_dep=
        filc_async_mark_pending(task, buf)               one per output buffer
        filc_async_mark_input(task, src)                 one per input buffer
        filc_async_submit(task, meta, run, staged, 4)    ─► meta->runtime->submit
        return task
...
c = buf[0]
  └─► if (header(buf).aux & PENDING)                    (emitted by FilPizlonator)
        filc_resolve_pending(buf, lower)                 ─► framework
          wait for every task owning the object
            task's runtime->poll(task, BLOCK)            ─► runtime
              ... filc_async_complete(task, result)      ◄─ runtime
          marks cleared, flag cleared, locks released
      load buf[0]
```

## Compiler

### FilAsync pass

`compiler/pass/FilAsync.cpp`

**Where it runs.** Clang records each `FILC_ASYNC` annotation in `llvm.global.annotations`.
`BackendUtil.cpp` installs FilAsync at the very start of Fil-C's pipeline,
before any optimization and before FilPizlonator. Keep that order: the
inliner and attribute inference must never see a direct call to an annotated
function, or they could fold or inline it before it is redirected
(`tests/compiler/t_pragma_same_tu_lazy.c`).

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
other units still link (`tests/io_uring/t_linked_async_*.c`).

**Parameter names.** Options name parameters (`bout=buf`, `r_dep=fd:file`).
IR declarations carry no parameter names, so a small clang patch
(`compiler/patches/filc-async-param-names.patch`) records them on
each annotated function as `!filc_async.params`, and the pass resolves each
option against that list. IR pointers carry no `const` either, so the same
patch records which parameters point to a `const` type as
`!filc_async.const`. The pass gives such a parameter, when no option names
it, the `bin=` kind.

**What it leaves alone.** The pass only validates parameter names and
dependency types. It never checks `op=`: the runtime decides which ops exist.

### The access hook

`compiler/patches/filpizlonator-pending-hook.patch`, a patch to Fil-C's
`FilPizlonator.cpp`

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

`runtime/framework/filc_async.c`, `filc_async_native.c`, `filc_async_arena.c`

**Tasks.** One per call. Running tasks are kept in a list, oldest first, and
tasks whose result has not been delivered are kept in a hash table of
handles.

**Pending marks.** A mark covers a whole Fil-C object,
`[zgetlower, zgetupper)`. Live objects never overlap, so marks are hashed by
the object's lower bound.

- **Write and read marks.** A call that produces an object holds a write
  mark on it; a call that only reads it (`bin=`, or a pointer to `const`)
  holds a read mark. One task holds at most one mark per object.
- **The header flag.** While an object has at least one write mark, the
  framework sets `FILC_OBJECT_FLAG_ASYNC_PENDING` (flag value 64, the one
  flag bit Fil-C leaves free) in the object header's `aux` word. It uses the
  same compare-and-swap loop Fil-C uses for its own flags. Read marks never
  set it: reading or writing a buffer that calls are only reading does not
  wait.
- **Why a flag.** Objects nothing is producing never reach the framework: the
  inline test fails and the access proceeds.
- **Exclusive marks.** Write marks made by a stub are exclusive. Marking an
  object another call owns first waits for that call, readers included, so
  two calls never write one buffer at once and a write never overtakes a
  call still reading it. A read mark never waits: a runtime about to hand
  the buffer to a device waits for its producers instead
  (`filc_async_wait_buffer`).
- **Calls made from a body.** A call made by a body running under
  `filc_async_run` does not wait for that body's own read marks; it would
  wait for the thread it runs on.
- **Shared marks.** Marks made by a runtime with `filc_async_mark_shared` can
  overlap.
- **Self-access.** A body running under `filc_async_run` ignores its own
  task's marks.

**Dependency locks.**

- **Keys.** One lock per (value, space). The value is an integer or an object
  base. The space is a hash of the dependency's `<param>:<namespace>`, plus
  the pointer bit.
- **Modes.** Readers share and writers exclude.
- **Order.** Requests queue in arrival order, so a stream of readers cannot
  starve a waiting writer.
- **Where calls wait.** A call that must wait does so in its stub, before
  reaching the runtime, by polling the holder.
- **Why locks and not a graph.** The runtime sees calls only in an order that
  is already safe, so no runtime needs to understand dependencies.

**Waiting.**

- **The only way to wait.** Every wait (for a lock holder, a buffer's owner,
  or `filc_async_wait`) is a loop of polls with `BLOCK`, through the runtime
  of the task being waited for.
- **Progress.** The framework never assumes the runtime makes progress on its
  own. A runtime that queues work, like io_uring, gets its chance to flush
  whenever someone needs a result.
- **Unsubmitted tasks.** If the task is not yet submitted, the framework waits
  on a condition variable instead of polling.

**Memory.** Tasks, marks, locks and staged arguments come from
`filc_async_alloc`, a GC-backed arena by default, so everything the kernel may
still reference stays reachable.

## Runtime

A runtime implements submit, poll and validate, exports them in its
descriptor, and reports results with `filc_async_complete`. Because the framework already did the locking and
marking, a runtime can be very small (see [Write a runtime](Writing-a-Runtime.md)).

The io_uring runtime:

- runs the body;
- waits for any input buffer still being produced;
- queues an SQE;
- sends queued SQEs to the kernel only when a poll needs a result.

A loop of calls therefore reaches the kernel in one batch. See
[The io_uring runtime](io_uring-Runtime.md).

### Several runtimes in one program

Runtimes never talk to each other. Everything that connects calls on
different runtimes is framework state:

- **Locks.** A `w_dep=` key taken by a call on one runtime makes a call on
  another runtime wait in its stub.
- **Pending buffers.** A buffer one runtime's call still owns makes an access
  from a body run by another runtime wait.
- **Waiting.** The waiter polls the owner through the owner's own runtime.

So a program can hand some functions to io_uring and others to, say, a
thread pool, and still order them with the usual annotations.

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
  (`tests/framework/t_thread_compute.c`).

**io_uring runtime.** It serializes on one recursive lock around a single
ring. That gives safety, not I/O parallelism across threads.

## Source map

| Area | Files |
|---|---|
| Call rewriting, descriptors, stubs, run thunks | `compiler/pass/FilAsync.cpp` |
| Pending-flag test at access sites | `compiler/patches/filpizlonator-pending-hook.patch` |
| Pass order | `compiler/patches/backend-util-run-filasync.patch` |
| Public headers | `runtime/include/`: `filc_async.h`, `filc_async_runtime.h`, `filc_async_alloc.h`, and io_uring's `fasync.h`, `fasync_dep.h` |
| Framework | `runtime/framework/filc_async.c` |
| Header flag and resolver bridge | `runtime/framework/filc_async_native.c` |
| Allocator | `runtime/framework/filc_async_arena.c` |
| io_uring runtime | `runtime/io_uring/`: `filc_async_uring.c`, the request layer `fasync*.c`, and its native half `fasync_native.c` |
| rpc runtime | `runtime/rpc/rpc_runtime.c` |
| Native forwarders | `runtime/patches/libpas-forwarders.patch` |
