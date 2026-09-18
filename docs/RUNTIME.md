# RUNTIME.md — the io_uring runtime, from the code up

This is the source-level reference for `runtime/src/`: which file owns what, why
the split is the way it is, and which invariants the code relies on. The design
rationale (mechanisms, trade-offs, measurements) lives in
[docs/ARCHITECTURE.md](ARCHITECTURE.md) and [idea.md](../idea.md). Some
documentation that used to live in source comments now lives here; the comments
that remain in the code are the small set that are load-bearing while reading
the function they annotate.

## The split

The runtime is one library in two halves, spliced into a private copy of
`libpizlo.a` (`runtime/build.sh`, see below).

| File | Compiler | Owns |
|------|----------|------|
| `fasync.c` | filcc (memory-safe) | ring setup/state, request table + free list, submission, resolution policy, `fasync_wait_all`, provenance, stats |
| `fasync_syscalls.c` | filcc | public syscalls (`fasync_pread/pwrite/fsync/close/openat/open_pending`), results (`fasync_ready/result/fd_resolve`), `fasync_req_wait`, pending-descriptor slots |
| `fasync_token.c` | filcc | token-ordered syscalls (`fasync_tagged_pread/pwrite`) |
| `fasync_dep.c` | filcc | declared effect sets, DAG construction/execution, tracker allocation |
| `fasync_native.c` | host clang (unsafe) | the three io_uring syscalls, the CQ drain, `filc_resolve_pending` — the trusted half |
| `fasync_internal.h` | — | private functions shared between fasync.c / fasync_syscalls.c / fasync_token.c: `fasync_push_sqe`, `fasync_push_buf`, `fasync_req_lookup`, `fasync_req_release`, `fasync_req_wait` |

Public entry points are in `fasync.h` (syscalls, results, provenance, stats)
and `fasync_dep.h` (effect sets, tokens). ABI definitions shared with the
native half are in `fasync_shared.h` (the resolver's view of the runtime) and
`fasync_io_uring.h` (the kernel ABI). `fasync_syscalls.h` declares the
io_uring syscalls and the safe-half → native-half bridge calls
(`fasync_publish_state`, `fasync_poll`, `fasync_block`).

## Why io_uring has to be native

Fil-C makes unsafe code structurally unlinkable, and io_uring cannot be reached
from memory-safe code:

- inline `syscall` asm in a filcc file is a compile-time safety error
  ("thwarted a futile attempt to violate memory safety"), and
- `zsys_syscall`'s dispatcher allowlists and rejects io_uring
  ("unsupported syscall: 425").

So the three syscalls are added the only way they can be: as first-class
pizlonated syscalls in the trusted runtime, exactly like `zsys_read` and
`zsys_openat`. The signatures are added to
`libpas/src/libpas/generate_pizlonated_forwarders.rb` — this repo carries the
modified generator at `runtime/upstream-overrides/`, which
`runtime/build.sh` installs over the vendored copy before regenerating the
`pizlonated_*` wrappers that memory-safe code actually calls. The build then
regenerates `filc_native.h` / `filc_native_forwarders.c` and splices the result
into the archive. The native implementations are `fasync_native.c`, and that
file is the only place raw `syscall` instructions exist.

The memory-safe half calls the native half through that same forwarder table:
`fasync_publish_state` (once, at ring setup), `fasync_poll` (reap, never
blocks), `fasync_block` (sleep until a completion). An earlier revision bridged
the whole resolution policy out to a native function taking a `filc_ptr`, and
it crashed intermittently in the generated argument/return marshalling; the
bridged surface is therefore scalars and void returns only.

## Why the compiler hook lands on native code

The whole point of the design is that the compiler inserts the resolution check,
not the programmer. This repo's modified FilPizlonator
(`compiler/upstream-overrides/FilPizlonator.cpp`, installed by
`compiler/build.sh`) emits a direct
call to `filc_resolve_pending` beside the capability check it already emits for
every access through an escaping pointer.

That call must land on *native* code. A function defined in filcc-compiled code
has a `pizlonated_*` entry point that is an 8-byte descriptor stub — calling it
directly returns a function object, not runs the body. Every other
compiler-emitted runtime call (`filc_check_function_call_fail` and friends) is
native for the same reason. So the *policy* is native (`filc_resolve_pending` in
`fasync_native.c`), while the *state* stays in the memory-safe half and is
published to the native side as raw pointers (see
`fasync_shared.h` — addresses and scalars only). The native side also drains the
completion ring and publishes queued batches, so the fast path never leaves
userspace.

## Ring setup

`fasync_ring_init` allocates the ring memory from the GC heap and hands it to
the kernel with `IORING_SETUP_NO_MMAP`. This is structural, not incidental:
Fil-C's `mmap` wrapper must hand back a capability for the address a mapping
lands at, so it pre-allocates the address and passes `MAP_FIXED` — and the
kernel rejects `MAP_FIXED` for io_uring ring mappings. With `NO_MMAP` the
caller supplies the memory, so the completion queue is ordinary GC memory that
memory-safe code can read directly. The one hazard is that the memory must not
move (the kernel caches the address): large page-aligned `zgc_aligned_alloc`
allocations are stable, and `stage2c_gc_pin_probe` checks it.

## Request table

- The table has `FASYNC_MAX_INFLIGHT` (= 1024) slots, sized so a whole workload
  fits in flight — a smaller table would force waves, which is the serialisation
  the design removes. The native resolver walks it, and the allocation bitmap
  (`fasync_shared.h`) is sized from it, so the constant is part of the shared
  contract.
- Allocation is a free list (a linear scan was O(in-flight) per request and
  ruined the many-small-operations case). Indices are one-based so 0 means
  "empty".
- A handle (the `fasync_id`) packs the slot index in the low 32 bits and a
  generation above it, so a handle held across a slot recycle is detected
  instead of silently resolving to an unrelated request.
- `g_inflight` is the fast-path gate: the resolver's first act is a single
  acquire load of it, and when nothing is in flight it returns without touching
  a lock, a table, or a syscall.
- `alloc_epoch` advances on every allocation and invalidates the resolver's
  negative-range memo (a freed buffer's address can be handed out again).
  Completions do not invalidate it — a request that finishes only shrinks
  pending coverage.

## Resolution

`filc_resolve_pending` (native, compiler-emitted) and `fasync_resolve_pending`
(explicit, from programs) implement the same policy: spin against the completion
ring (a plain shared-memory read) up to a budget, then park in the kernel,
asking for events (`min_complete=1, IORING_ENTER_GETEVENTS` — without
GETEVENTS the kernel processes `to_submit` and returns, ignoring
`min_complete`, so a wait loop would spin through enters instead of sleeping
once).

Blocking enters take the GC safepoint around the syscall (`filc_exit` /
`filc_enter`); non-blocking submits do not, because a stop-the-world handshake
on the submission fast path would defeat the point of zero-context-switch
submission.

Nothing requires an explicit `fasync_submit()`. The safe half can queue SQEs
the kernel never sees; the first resolution that genuinely needs a completion
(`fasync_resolve_pending`, `fasync_req_wait`, or the native hook) publishes the
whole queued batch in one non-blocking enter. `fasync_wait_all` collects that
batch draining the ring and sleeping only when there is nothing to reap.

The resolver only looks for a *contained* range: the emitted hook passes
`size = 1`, on the reasoning that a derived pointer into a pending buffer always
starts inside it, so containment of its first byte is what detection needs. A
pointer that starts outside a pending buffer and straddles into it is not
detected (a documented limitation; see ARCHITECTURE.md §7).

## Descriptors and eager writes

A pending open returns a *pending descriptor* — a negative handle, encoded as
`-(index + 2)` so that -1 stays an unambiguous failure value and slot 0 does not
alias it. Every fd-taking op resolves it first (`fasync_fd_resolve`), which is
how using a not-yet-open descriptor creates the dependency.

`fasync_pwrite` resolves its source eagerly rather than lazily: the kernel reads
the source bytes when it executes the SQE, and there is no way to tell it "use
whatever lands here", so a write whose source is a prior async read is a genuine
synchronisation point. That costs the overlap between the write and the read
that feeds it — the exact case the design cannot get for free (see
ARCHITECTURE.md §5).

`fasync_openat_direct` — openat into an explicit direct-descriptor slot so a
dependent op can submit against an fd that does not exist yet — is deliberately
absent: it is only meaningful with a registered file table, and `stage6b_fd_chain_probe`
documents what the kernel does with an explicit slot when there is none. It
arrives with the promise-pipelining work.

## Tokens

A serialization token (async-a-sync.pdf) is a named resource claimed INOUT by
everyone who shares it — the ordinary dependency machinery over a non-address
resource, which is why it needs no separate code path in the DAG layer. The
direct runtime form (`fasync_tagged_pread/pwrite`) instead orders calls as they
are issued: a tagged call is not enqueued until every *conflicting* tagged call
already issued on the token has finished, where "conflicting" means at least one
side writes (reads overlap freely). A token's queue holds up to
`FASYNC_TOKEN_QLEN` (16) in-flight ops before the oldest is waited out to make
room. Both forms share the `fasync_tracker` struct.

## Build and the two upstream overrides

`runtime/build.sh` compiles the five runtime units (forwarders + native half
with host clang; the rest with filcc) and splices them into a private copy of
the distributed `libpizlo.a`. The rest of libpizlo is reused as-is — the
extension is additive. Two upstream files have to change to carry it, and this
repo tracks the modified versions in `upstream-overrides/` directories, which
the build scripts install over the vendored copy (idempotently) before building.
That keeps the delta inside the repo — it clones, builds, and reproduces on its
own — without carrying a patch file:

- `runtime/upstream-overrides/generate_pizlonated_forwarders.rb` — the forwarders
  generator with the `zsys_io_uring_*` and runtime-bridge signatures added;
  `runtime/build.sh` installs it and regenerates the forwarders from it.
- `compiler/upstream-overrides/FilPizlonator.cpp` — the pass modified to emit
  `filc_resolve_pending`; `compiler/build.sh` installs it before the clang
  build, documented in `compiler/README.md`.

Each is byte-identical to stock upstream apart from its change, so diffing it
against upstream's file shows exactly what the project modifies.

The source-built patched clang finds its Fil-C runtime at
`<binary>/../../../pizfix` (i.e. `<repo>/vendor/pizfix`); `tests/run.sh` creates
that symlink against the distribution's pizfix when the patched compiler is
present. Programs never use the patched compiler, the split is what keeps the
runtime identical either way: the compiler decides *where* resolution is
needed, the runtime decides *what* resolution means.