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
   optimizations and `FilPizlonatorPass`; keep this order. The annotated body
   is a stub the backend never runs, so the inliner and attribute inference
   must not see direct calls to it (see `tests/t_pragma_same_tu_lazy.c`). The pass
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
3. Direct calls to annotated functions are rewritten to allocate staged
   argument cells, mark producing buffers pending, and call
   `filc_async_submit(meta, impl, opts, args, nargs)`. The pass preserves the
   ordinary linker name of an annotated declaration so its implementation
   can be in another translation unit. A definition in the same unit is
   renamed to `__filc_async_<name>`, and a non-static one keeps `<name>` as
   an alias of it, so callers in other units still link when the definition
   is annotated too (the usual case with an annotated header). Its body
   remains linkable but the io_uring backend never executes it.
4. Only direct call sites with pointer or void returns are rewritten. A
   non-void scalar return produces a diagnostic and remains a direct call.
   The pass does not rewrite indirect calls. Declaration and definition
   annotations follow the same option handling; if both are annotated, the
   definition's options apply. See `tests/check_dependency_options.sh`.
5. `FilPizlonatorPass` widens pointer operations to Fil-C capabilities and
   calls `filc_resolve_pending(ptr, 1)` before capability checks on escaping
   pointer accesses. The hook is intentionally at the access site. It does
   not change the pointer address: the kernel writes into the original
   buffer. The native hook in `runtime/src/fasync_native.c` uses an in-flight
   counter for its fast path, searches for a covering request, publishes the
   queued batch, drains CQEs in userspace, then blocks at a safepoint if
   spinning does not finish the request.

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
The runtime's `filc_async_arg` and the direct-submit fixture in
`tests/t_backend_io_uring.c` therefore use explicit 16-byte structs. An
8-byte union stride reads the wrong cell: a length of 6 was interpreted as
larger than `UINT_MAX`, returning `-EOVERFLOW` before any SQE was queued.
Both C structs have a size assertion to catch that regression.

`filc_async_submit` returns a task pointer. `filc_async_poll` and
`filc_async_wait` accept it in `filc_async_result_s.pending`, then fill
`result` with a byte count, fd, zero, or negative errno and `state` with
0 (done), 1 (pending), or 2 (failed).

### Runtime side

`runtime/build.sh` installs
`runtime/upstream-overrides/generate_pizlonated_forwarders.rb` and regenerates
native forwarders for `zsys_io_uring_*`,
compiles the syscall bridge and runtime, and splices those objects into a
private `runtime/build/lib/libpizlo.a`. The bridge in
`runtime/src/fasync_native.c` checks Fil-C capabilities before passing raw
addresses to the kernel. The safe side in `runtime/src/fasync.c` allocates
GC-pinned, page-aligned ring memory and uses `IORING_SETUP_NO_MMAP`; its SQEs
are queued first and normally published to the kernel on demand. The request
table is shared with the native access hook. `runtime/src/fasync_syscalls.c`
validates buffer and path bounds, handles supported operations, and provides
negative temporary fd handles for pending opens in the explicit API.

For the annotated API, `runtime/src/filc_async.c` validates each descriptor's
argument kinds at startup. Supported syscall shapes are `pread(fd, buf,
len, offset)`, `pwrite(fd, buf, len, offset)`, `openat(dirfd, path, flags,
mode)`, `fsync(fd)`, and `close(fd)`. The compiler treats `op=` as an option
string; the runtime decides whether the operation exists.

The pass marks `bout=`, bare `buf=`, and unannotated pointer arguments as
pending before submission. `bin=` is an input and is not marked. The
runtime's pending registry compares Fil-C object ranges using `zgetlower`
and `zgetupper`. A new mark overlapping an earlier one resolves that owner
first. Completed annotated tasks retire their marks and request slots when
observed through poll, wait, or a later pending-state check. A program that
only touches its buffers never does any of those, so when the request table or
the mark registry is full the runtime completes the finished tasks itself, or
waits for the oldest one (`tests/t_pragma_lazy_many.c`). In-flight tasks sit
on one list, which dependency checks walk; completed ones move to a second
list until poll/wait delivers them. The staged argument allocation
and task are kept reachable so the kernel's borrowed buffer pointers remain
valid while requests are in flight.

### Dependency ordering

Repeat `r_dep=<argument index>` and `w_dep=<argument index>` to describe a
call's reads and writes. Each key is the argument value: integer keys compare
as 64-bit values; pointer keys compare by Fil-C object base and cannot match
an integer key. Matching read/read claims can overlap. Matching claims where
either side writes are ordered by call submission order.

The task list is newest first; a task scans its `next` chain for earlier
conflicts. `g_dependency_mutex` guards task insertion, predecessor checks,
request start, and completion/claim retirement. The completion path retires
the task and starts newly ready successors. The mutex is released before
waiting for an earlier request. `filc_async_submit` may therefore wait for a
conflicting predecessor: it does not return a task until the new task has
started and has a request the access hook can find. Independent work can
still be queued together. This policy is separate from the explicit API's
effect-set DAG in `fasync_dep.c`.

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
| `tests/stage4_compiler_hook.c` and `stage8_latency.c` | The compiler's access hook resolves pending buffers; the latter also measures latency. |

Run `./tests/run.sh` after both builds. It skips patched-compiler cases if
`vendor/fil-c-src/build/bin/filcc` is absent, so verify the final count and
skip line. A full run on 2026-09-29 passed 38 tests with zero failures in an
environment that permitted io_uring. In a restricted sandbox,
`io_uring_setup` returned `EPERM` and many unrelated tests failed at request
creation. This is an environment failure, not evidence that dispatch is
incorrect. See `compiler/README.md` for build and link troubleshooting.

## Limitations and next checks

- Only the five syscall shapes above are implemented. Annotated function
  bodies do not run on this backend. Indirect calls and non-void scalar
  return call sites are not lowered.
- Only code compiled by the patched compiler resolves pending buffers on
  access. Fil-C's libc is not, so `memcmp`, `strlen`, `write` and the like
  read a pending buffer as it stands; touch it first or poll/wait.
- The access hook checks one byte at the access pointer. A pointer beginning
  outside a pending buffer and straddling into it is not detected by that
  lookup. Review this before broadening memory operations.
- At most 1024 requests and 1024 pending marks can be outstanding at once;
  beyond that an annotated call waits for an earlier one. The explicit
  pending-fd table has 64 entries. Completed annotated tasks that are never
  polled or waited on stay allocated in the arena. There is no general fallback when io_uring is blocked.
- `./tests/run.sh` covers `demos/run_wordcount.sh`, but it does not run the
  Makefile targets, `demos/run_wordcount_3way.sh`, or
  `demos/inspect_disasm_cfg.sh`. Their link commands carry the trailing
  `-lpizlo -lc`; run them by hand after changing the runtime.
- The `deluge` branch can move. The upstream SROA patch and compiler
  overrides are tied to the tested source shape; a future source revision
  may require rebasing them. The tested source revision is recorded in
  `compiler/README.md`.
