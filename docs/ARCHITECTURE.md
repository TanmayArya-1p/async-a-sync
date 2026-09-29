# Implementation guide

This describes the annotated io_uring implementation as tested on
2026-09-29. Start with the root `README.md` for the programming model and
`compiler/README.md` for the exact Fil-C setup and failure signatures.
The build artifacts and `vendor/` checkout are ignored by Git; a checkout of
this repository provides neither compiler binary nor the Fil-C distribution.

## Two paths into the same io_uring runtime

The explicit `fasync_*` API in `runtime/src/fasync.h` issues requests directly.
The `filc_async` pragma path turns annotated C calls into requests through
`filc_async_submit`, then uses the same `fasync_*` operations and ring. The
generic effect-set/DAG code in `runtime/src/fasync_dep.c` is distinct from the
argument-key ordering used by annotated calls in `runtime/src/filc_async.c`.

The main path for one annotated call is:

```text
Clang function annotation
  -> FilAsyncPass metadata, 16-byte argument cells, and filc_async_submit
  -> FilPizlonator pointer lowering and access hook
  -> filc_async_submit task and dependency gate
  -> fasync_* SQE queued in io_uring
  -> poll/wait or first access publishes and resolves the request
```

### Compiler side

1. Clang emits `llvm.global.annotations` for a pragma around a function
   declaration or definition. `clang/lib/CodeGen/BackendUtil.cpp` installs
   `FilAsyncPass` at the start of Fil-C's pipeline, **before** its early
   optimizations and `FilPizlonatorPass`; keep this order. The inliner and
   attribute inference must not see direct calls to an annotated function
   before they are redirected to its stub, or they could fold or inline them
   (see `tests/t_pragma_same_tu_lazy.c` and `tests/t_pragma_repeat_read.c`). The pass
   reads the annotation's `op=`, `fd=`, `bin=`, `bout=`, `buf=`, `r_dep=`, and
   `w_dep=` strings. `read_dep=` and `write_dep=` are rejected. A pragma around
   a call gets Clang's unused-attribute warning; compile with
   `-Werror=pragma-clang-attribute` to make it an error.
2. `FilAsyncPass` emits an options array, a `filc_async_meta` descriptor, and
   a per-translation-unit table. A late constructor calls
   `filc_async_validate_table` before `main`. The pass checks positional
   argument indices and dependency types. The runtime validator owns the
   supported `op=` set and operation shapes; an unknown op is rejected at
   startup. `op=ignore` is a test-only accepted operation.
3. Direct calls to annotated functions are redirected to a stub the pass
   emits for each function. The stub stages the arguments in 16-byte cells,
   starts a task with `filc_async_begin`, takes one `filc_async_lock_word` or
   `filc_async_lock_ptr` per dependency argument and one
   `filc_async_mark_pending` per output buffer, and hands the call to the
   runtime with `filc_async_submit(task, meta, run, args, nargs)`, where
   `run` is a thunk that calls the body with the staged arguments. The
   framework (`runtime/src/filc_async.c`) implements the stub's calls and
   knows nothing about ops; the runtime implements `filc_async_submit`,
   `filc_async_runtime_poll` and `filc_async_runtime_validate`
   (`runtime/src/filc_async_runtime.h`). The pass preserves the
   ordinary linker name of an annotated declaration so its implementation
   can be in another translation unit. A definition in the same unit is
   renamed to `__filc_async_<name>`, and a non-static one keeps `<name>` as
   an alias of it, so callers in other units still link when the definition
   is annotated too (the usual case with an annotated header). The io_uring
   runtime (`runtime/src/filc_async_uring.c`) runs the body through the run
   thunk before it issues the request.
4. Only direct call sites with pointer or void returns are rewritten. A
   non-void scalar return produces a diagnostic and remains a direct call.
   The pass does not rewrite indirect calls. Declaration and definition
   annotations follow the same option handling; if both are annotated, the
   definition's options apply. See `tests/check_dependency_options.sh`.
5. `FilPizlonatorPass` widens pointer operations to Fil-C capabilities and,
   before the capability check on an escaping pointer access, tests the
   async framework's pending flag in the header of the object the pointer's
   capability names. Only when the flag is set does it call
   `filc_resolve_pending(ptr, lower)`. The hook is intentionally at the access
   site. It does not change the pointer address: the kernel writes into the
   original buffer. The native hook in `runtime/src/filc_async_native.c`
   calls the framework's resolver, which waits, through the runtime's poll,
   for every call that still owns the object.

### ABI between compiler and runtime

`filc_async_meta` in `runtime/src/filc_async.h` must match `FilAsyncPass`'s
emitted descriptor field for field. The header records the Fil-C layout
offsets: `name` at 0, `nargs` at 16, `opts` at 32, and the argument metadata
array at 48. Kinds and dependency bits in the pass mirror the constants in
the header. If adding an option or field, update both sides and the metadata
tests together.

The pass allocates **16 bytes per staged argument**. Scalars use the first
64-bit word; pointer stores keep the Fil-C capability. Although Fil-C's
lowered pointer occupies a 16-byte cell, `sizeof(void*)` in C source is 8.
The io_uring runtime's `staged_arg` and the direct-submit fixture in
`tests/t_backend_io_uring.c` therefore use explicit 16-byte structs. An
8-byte union stride reads the wrong cell: a length of 6 was interpreted as
larger than `UINT_MAX`, returning `-EOVERFLOW` before any SQE was queued.
Both C structs have a size assertion to catch that regression.

An annotated call returns its task, which the stub got from
`filc_async_begin`. `filc_async_poll` and `filc_async_wait` accept it in
`filc_async_result_s.pending`, then fill `result` with the value the runtime
reported (for io_uring a byte count, fd, zero, or negative errno) and `state`
with 0 (done), 1 (pending), or 2 (failed).

The interface between the framework and a runtime is
`runtime/src/filc_async_runtime.h`. A runtime implements
`filc_async_submit(task, meta, run, args, nargs)`,
`filc_async_runtime_poll(task, mode)` and `filc_async_runtime_validate(meta)`;
poll's mode says whether it may only look (check), start deferred work
(progress) or block. The framework gives runtimes `filc_async_complete`,
`filc_async_run` (runs a body, so the body may use its own pending buffers),
`filc_async_resolve_buffer`, `filc_async_mark_shared`,
`filc_async_wait_buffer`, and a per-task word for the runtime's state. A
runtime that ran each body on a worker thread would only need submit to
queue `run` and report its return value; `tests/mock_runtime.c` is a
synchronous one.

### Runtime side

`runtime/build.sh` installs
`runtime/upstream-overrides/generate_pizlonated_forwarders.rb` and regenerates
native forwarders for `zsys_io_uring_*` and the framework's `zasync_*`
functions. It splices the framework (`filc_async.c`, the arena,
`filc_async_native.c`) and the native bridges into a private
`runtime/build/lib/libpizlo.a`, and archives the io_uring runtime
(`fasync*.c`, `filc_async_uring.c`) as
`runtime/build/lib/libfilc_async_uring.a`. The bridge in
`runtime/src/fasync_native.c` checks Fil-C capabilities before passing raw
addresses to the kernel. The safe side in `runtime/src/fasync.c` allocates
GC-pinned, page-aligned ring memory and uses `IORING_SETUP_NO_MMAP`; its SQEs
are queued first and normally published to the kernel on demand. The request
table is shared with the native completion drain. `runtime/src/fasync_syscalls.c`
validates buffer and path bounds, handles supported operations, and provides
negative temporary fd handles for pending opens in the explicit API.

For the annotated API, the io_uring runtime (`runtime/src/filc_async_uring.c`)
validates each descriptor's argument kinds at startup. Supported syscall shapes are `pread(fd, buf,
len, offset)`, `pwrite(fd, buf, len, offset)`, `openat(dirfd, path, flags,
mode)`, `fsync(fd)`, and `close(fd)`. The compiler treats `op=` as an option
string; the runtime decides whether the operation exists.

The stub marks `bout=`, bare `buf=`, and unannotated pointer arguments
pending before it hands the call to the runtime. The pass only accepts
`bin=`, `bout=` and `buf=` on pointer arguments. `bin=` is an input and is
not marked. A mark covers the whole Fil-C object (`zgetlower` to `zgetupper`)
and sets the object's pending flag, which the access hook tests; marking an
object another call owns waits for that call first. The explicit `fasync_pread`
marks its buffer too, with a shared mark, so several explicit reads can own
one object. When a call completes, its marks and locks go. A program that
only touches its buffers never polls, so when the io_uring request table is
full the runtime completes the finished calls itself, or waits for the oldest
one (`tests/t_pragma_lazy_many.c`). The staged argument allocation and task
are kept reachable so the kernel's borrowed buffer pointers remain valid
while requests are in flight.

### Dependency ordering

Repeat `r_dep=<argument index>` and `w_dep=<argument index>` to describe a
call's reads and writes. Each key is the argument value: integer keys compare
as 64-bit values; pointer keys compare by Fil-C object base and cannot match
an integer key. Matching read/read claims can overlap. Matching claims where
either side writes are ordered by call submission order.

A `:<name>` suffix, as in `w_dep=0:meta`, puts the key in a namespace: the
pass hashes the name to 24 bits in bits 8..31 of the dependency word, and
equal values in different namespaces are different resources.

The stub locks each dependency argument's value, in its namespace, before
it hands the call to the runtime: a read lock is shared and a write lock is
exclusive. A call that needs a lock another call holds in a conflicting mode
waits in its stub, polling the holder through the runtime, and each lock
grants its requests in arrival order. A call's locks are released when it
completes. Independent calls queue together. This is separate from the
explicit API's effect-set DAG in `fasync_dep.c`.

### Threading

Any thread may make annotated calls, wait on them and touch their buffers.
The framework (`filc_async.c`) guards its tasks, locks and marks with one
mutex and signals a condition variable when a task completes or a dependency
lock changes hands; it never holds that mutex while it calls the runtime, so a
runtime may report completions from any thread. Dependency locks queue their
requests and grant them in arrival order. The io_uring runtime guards its
ring, request table and pending-fd table with one recursive lock, which it
never holds while running a body or waiting for a buffer through the
framework; the lock adds safety, not parallelism, since every thread's
requests share the ring. The arena allocator has its own lock. A thread that
only computes on its own memory never finds a pending flag, so it never calls
into the framework (`tests/t_threads.c`, `tests/t_thread_compute.c`).

## Validation map

| Check | What it proves |
|---|---|
| `tests/check_dependency_options.sh` | Pass metadata, option precedence, and rejected old dependency names. |
| `tests/check_callsite_pragma.sh` | A pragma around a call is a compile error with the required warning flag. |
| `tests/check_dependencies.sh` | Deterministic read/write ordering against a fake request backend. |
| `tests/check_forwarders.sh` | Native io_uring and completion forwarder symbols are present. |
| `tests/check_linkage.sh` with `t_linked_async_*` | A separately defined function, generated runtime symbols, and the final binary link and execute. |
| `tests/t_backend_io_uring.c` | Direct `filc_async_submit` calls reach the supported io_uring operations. |
| `tests/t_pragma_io_uring.c` and `t_pragma_dependencies.c` | Annotated calls dispatch and honor dependency order end to end. |
| `tests/t_threads.c` and `t_thread_compute.c` | Two threads make annotated calls and read each other's buffers; a thread that only computes stays out of the runtime. |
| `tests/stage4_compiler_hook.c` and `stage8_latency.c` | The compiler's access hook resolves pending buffers; the latter also measures latency. |

Run `./tests/run.sh` after both builds. It skips patched-compiler cases if
`vendor/fil-c-src/build/bin/filcc` is absent, so verify the final count and
skip line. A full run on 2026-09-29 passed 42 tests with zero failures in an
environment that permitted io_uring. In a restricted sandbox,
`io_uring_setup` returned `EPERM` and many unrelated tests failed at request
creation. This is an environment failure, not evidence that dispatch is
incorrect. See `compiler/README.md` for build and link troubleshooting.

## Limitations and next checks

- The io_uring runtime implements only the five syscall shapes above.
  Indirect calls and non-void scalar return call sites are not lowered.
- Only code compiled by the patched compiler resolves pending buffers on
  access. Fil-C's libc is not, so `memcmp`, `strlen`, `write` and the like
  read a pending buffer as it stands; touch it first or poll/wait.
- At most 1024 io_uring requests can be outstanding at once; beyond that an
  annotated call waits for an earlier one. The explicit
  pending-fd table has 64 entries. Completed annotated tasks that are never
  polled or waited on stay allocated in the arena. There is no general fallback when io_uring is blocked.
- `./tests/run.sh` covers `demos/run_wordcount.sh`, but it does not run the
  Makefile targets, `demos/run_wordcount_3way.sh`, or
  `demos/inspect_disasm_cfg.sh`. Their link commands carry the trailing
  `-lpizlo -lfilc_async_uring -lpizlo -lc`; run them by hand after changing
  the runtime.
- The io_uring runtime serializes all threads' I/O on one ring. A ring per
  thread is what would scale I/O across cores.
- The `deluge` branch can move. The upstream SROA patch and compiler
  overrides are tied to the tested source shape; a future source revision
  may require rebasing them. The tested source revision is recorded in
  `compiler/README.md`.
