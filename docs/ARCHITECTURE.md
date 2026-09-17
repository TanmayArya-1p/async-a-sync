# Architecture

Transparent async syscalls on Fil-C, via pointer provenance tracking and lazy
resolution. This document describes what was built, how it works, and — at least
as importantly — what was measured and what did not work the way `idea.md`
expected.

The goal, from `idea.md` §1: make syscalls asynchronous **without changing
call-site syntax**. A syscall becomes an `io_uring` submission, the value it
produces carries a tag identifying the pending request, and the first *genuine
access* through that value (or anything derived from it) resolves it
transparently. No `.await`, no futures, no explicit yield points.

---

## 1. What is actually built, and what is verified

| Piece | Status |
|---|---|
| io_uring driver inside the Fil-C trusted runtime | **working, tested** |
| `AsyncPtr`-style lazy resolution (`filc_resolve_pending`) | **working, tested** |
| Provenance: derived pointers keep their request tag | **working, tested** |
| Spin-then-park resolution against the completion queue | **working, tested** |
| Declared effect sets + capability-range disjointness proof | **working, tested** |
| Dependency DAG construction and execution | **working, tested** |
| Serialization tokens (`async-a-sync.pdf`) | **working, tested** |
| Descriptor provenance (ops against a not-yet-open fd) | **working, tested** |
| FilPizlonator patch that inserts the resolution hook | **working, tested** |
| Kernel-native fd chaining (`IOSQE_IO_LINK` + direct descriptors) | **blocked by the kernel here** |
| Benchmark against `tokio-uring` / `monoio` (`idea.md` §6 phase 6) | **not done** |

"Working" for the compiler row means end to end: clang was built from the patched
sources, it emits the hook, and a program compiled with it resolves async results
with no explicit API call at the access site (`tests/stage4_compiler_hook.c`).

That last part is not a detail, because getting there forced an architectural
change. The resolution *policy* had to move into the trusted runtime, for the
reason in §2.5.

---

## 2. The substrate: what Fil-C actually does

`idea.md` §2.6 proposes Fil-C as the implementation substrate. The survey work
(`idea.md` §6 phase 2) turned up four facts that shaped everything downstream.
Three of them were verified by experiment rather than by reading documentation.

### 2.1 There is genuinely no escape hatch — verified

Fil-C's headline claim is that no unsafe code can be linked outside a small
trusted runtime core. That is enforced by the compiler, not just by convention.
Attempting to issue a raw `syscall` instruction from memory-safe code:

```
filc safety error: cannot handle inline asm (unsupported mnemonic for
safe inline asm: syscall)
filc panic: thwarted a futile attempt to violate memory safety.
```

And routing around it via `syscall(2)` does not help either — `zsys_syscall`
dispatches through an allowlist and rejects anything it does not know:

```
filc user error: unsupported syscall: 425.
    (libpizlo.so) ../filc/src/runtime.c:656: zsys_syscall
```

**This closes a risk that `idea.md` §5 lists as open.** §5 worries that "any
boundary the instrumentation can't see through (FFI, erasure, in Rust's case
`unsafe`) silently breaks the illusion". On Fil-C there is no such boundary to
leak through: the language has no `unsafe`, inline assembly is screened against a
safe-mnemonic whitelist, and the only code that can issue arbitrary syscalls is
the trusted runtime, which is a single auditable component. The leak that §5
fears is, on this substrate, not merely closed but closed *structurally*.

### 2.2 The checks are inlined, so the hook must be in the compiler

`FilPizlonator.cpp:3788-3825` shows how an access is instrumented: an inline
comparison branching to an out-of-line `RangeFailB` block.

```
if (!expect_true(ptr_below_upper)) goto RangeFailB;   // inline, on every access
... the access itself proceeds inline ...
RangeFailB: call filc_*_check_fail();                 // out-of-line, aborts
```

Only the *failure* path is a callable symbol. The success path — the one that
fires on every ordinary access — is inline machine code with nothing to
interpose on. So `idea.md` §2.6's "reuse the check FilPizlonator already inserts"
is correct in spirit but does require modifying the pass; it cannot be done from
`libpizlo` alone.

### 2.3 `mmap` cannot map an io_uring ring — and why that is structural

The natural way to build a ring is: let the kernel create it, `mmap` the SQ/CQ
rings and the SQE array. That cannot work here, and the reason is not a bug on
either side.

Fil-C's `mmap` wrapper must guarantee that the address a mapping lands at is the
address it hands back a capability for, so it pre-allocates the address out of
the GC heap and passes `MAP_FIXED`. The kernel rejects `MAP_FIXED` for io_uring
ring mappings (`tests/stage2b_mmap_probe.c`):

```
mmap(NULL,  640, PROT_READ|PROT_WRITE, MAP_SHARED|MAP_POPULATE, fd, 0)
    -> succeeds
mmap(addr,  640, PROT_READ|PROT_WRITE, MAP_SHARED|MAP_POPULATE|MAP_FIXED, fd, 0)
    -> EINVAL
```

The consequence is sharp: **a ring mapping can never carry a Fil-C capability**,
so memory-safe code could never read the completion queue out of one. The
architecture has to account for that, and §3 below is the answer.

### 2.4 Large GC allocations do not move — verified

Because of 2.3, the runtime supplies the ring memory itself out of ordinary GC
memory (see §4). That is only sound if the collector never relocates it, since
the kernel caches the address. `tests/stage2c_gc_pin_probe.c` checks this
directly: allocate page-aligned, force collection cycles and explicit
scavenges, verify the address and contents are unchanged. They are.

### 2.5 Compiler-emitted hooks must be *native* — verified the hard way

This one cost real debugging time and shaped the final architecture, so it is
worth stating precisely.

The patched pass emits a direct call to a runtime function. The obvious choice of
target is the memory-safe implementation, but that does not work, and the way it
fails is instructive rather than obvious:

* A function defined in filcc-compiled code has a `pizlonated_*` entry point
  which is not the function. It is an **8-byte descriptor stub**:

  ```
  pizlonated_fasync_resolve_pending:
    lea   0xf5999(%rip),%rax
    mov   %rax,%rdx
    ret
  ```

  Calling it directly returns a *function object*, and running the body requires
  the closure/calling-convention protocol that the frontend emits. The compiled
  binary contained the call, executed it on every one of 4096 accesses, and the
  function body never ran once.

* Routing through a native shim does not help either, because that trades one
  unsupported direction for another: native code cannot simply call into
  memory-safe code.

* What does work is what Fil-C already does for its own compiler-emitted calls.
  Names beginning with `filc_` are referenced *unprefixed* and called directly,
  which is exactly what a pass can emit — and every function the pass already
  calls (`filc_check_function_call_fail`, `filc_cc_rets_check_failure`, and the
  rest) is **native**.

So the conclusion is a rule: **anything the compiler emits a call to must be
implemented in the trusted runtime.** The resolution policy therefore lives in
`fasync_native.c`, and the memory-safe half keeps only the capability checks and
the bookkeeping. The two halves share the request-table layout through
`fasync_shared.h`, and the safe half publishes raw addresses into its state, which
is sound because that memory does not move (§2.4).

### 2.6 A bridged function taking a `filc_ptr`, called in a loop, crashed

Worth recording because it was intermittent and the symptom pointed nowhere near
the cause.

The first version of the safe-side wait called a bridged native function
`fasync_resolve(filc_ptr, size_t) -> filc_ptr` inside its retry loop. That
segfaulted about 25% of runs, always with no output from the native function's own
first statement -- so the crash was in the generated argument/return marshalling,
before the body ran. Removing that one call took the failure rate to 0/20.

The root cause was not chased to the bottom of the generated code, because the fix
is better anyway: **the bridged surface is now scalars and void returns only.** The
wait policy moved back to the safe side, where it is expressed with the two
scalar bridged calls it actually needs (`fasync_poll` to drain without a syscall,
`fasync_block` to sleep). The compiler-inserted hook is unaffected -- it calls the
native `filc_resolve_pending` directly and keeps its tight spin -- so nothing on
the hot path got slower.

---

## 3. Shape of the system

The extension is **additive**: no Fil-C source file is modified for the runtime
itself. It adds object files to a private copy of the distributed `libpizlo.a`.

```
                    memory-safe (filcc)          trusted / unsafe (host clang)
                    ───────────────────          ────────────────────────────
  demos, tests  ──► fasync.c                      fasync_native.c
                    · ring allocation (GC mem)     · raw io_uring syscalls
                    · request table                · GC safepoint bracketing
                    · capability checks (zcheck)   · capability checks on args
                    · provenance                   · RESOLUTION POLICY
                    · SQEs / ops                   · filc_resolve_pending
                    fasync_dep.c                   (libpas forwarder table,
                    · effect sets                   regenerated)
                    · disjointness proof
                    · DAG + scheduler
```

Resolution sits on the right-hand side, and §2.5 is why: the compiler emits a
call to `filc_resolve_pending`, so that function has to be native. The memory-safe
half still *owns* the state — the ring and the request table are its memory — and
publishes raw addresses into it through `fasync_shared.h`, so both halves see one
request table and one ring. Its own explicit-access path calls into the same
native policy, so there is exactly one implementation of resolution and the two
paths cannot diverge.

Two halves, because Fil-C forces it. Anything that issues a syscall the runtime
does not already expose must live in the trusted core, and anything that touches
program memory must be capability-checked, which only the safe half can do. The
boundary between them carries only scalars and addresses — never a Fil-C pointer
that the trusted half would have to dereference.

**How `zsys_io_uring_*` becomes callable.** Fil-C's own mechanism for bridging a
new syscall is
`libpas/src/libpas/generate_pizlonated_forwarders.rb`, which holds a
hand-maintained signature list and generates the `pizlonated_*` wrapper that
memory-safe code calls into. The extension adds three signatures
(`runtime/patches/0001-libpas-io_uring-forwarders.patch`), regenerates the
forwarder table, and implements the `filc_native_*` bodies in `fasync_native.c`.
That is the sanctioned extension point for this layer, and it is where `idea.md`
§2.6's "hook in libpizlo where syscalls are already recognized as a checked
boundary" actually lands.

---

## 4. The three mechanisms

### 4.1 Zero-context-switch submission

`fasync_pread` and friends write an SQE into shared memory and return a handle.
They do not wait and they do not enter the kernel. Publication happens once per
batch in `fasync_submit()`, with a single `io_uring_enter(to_submit, min_complete
= 0)` — which does not block.

Measured, from `demos/demo_async_io.c`: 64 reads across 16 MiB enqueued *and*
published in **0.049 ms**, with **zero** blocking kernel entries. The blocking
loop doing the same reads pays 64 separate round trips.

The `min_complete > 0` case is the only one that can sleep, and it is wrapped in
the GC safepoint protocol (`filc_exit`/`filc_enter`) so a collector handshake can
never deadlock against a thread parked in the kernel. The non-blocking path
deliberately does *not* pay that cost, which is the whole point.

### 4.2 Provenance tracking

A request handle identifies the request, not the buffer. Anything derived from a
pending result inherits the tag, and `fasync_provenance()` recovers it for a
derived pointer by locating the pending range that contains it. Derived pointers
therefore resolve correctly whether the access is to the buffer's first byte or
to a field a thousand bytes in.

### 4.3 Lazy resolution on first genuine access

`filc_resolve_pending(ptr, size)` is the hot path. It lives in the native half
(§2.5), and it runs on every instrumented access, so its cost when nothing is
pending is the design's most important number:

```c
if (__atomic_load_n(&g_inflight, __ATOMIC_ACQUIRE) == 0)
    return ptr;                 /* one load, one predicted branch */
```

Only if something is in flight does it look for a covering pending range, and
only then does it poll the completion ring. Polling is a plain read of shared
memory: no syscall, no mode switch. If the completion has not landed, it spins
against the ring and parks only as a last resort.

Measured: submitting and resolving 64 reads produced **281,832 userspace
completion-ring polls and zero parks**, and resolving 8 reads produced 3,737
polls with zero parks. In the workloads exercised, the completion was always
there by the time anyone looked.

**Resolution flips the pending bit in place; it does not move data.** The kernel
was already told where to write, so the buffer address never changes. This is one
of the two options `idea.md` §2.6 offers ("swap the capability to point at the
real buffer, or flip the pending bit in place"), and choosing it has a payoff
beyond simplicity: because the pointer's value is unchanged, the compiler patch
in §6 does not have to rebind anything — it only has to guarantee resolution
happened first, which is a much smaller and safer change.

### 4.4 Ring memory

Because of §2.3, the runtime does not mmap the ring. It sets
`IORING_SETUP_NO_MMAP` (kernel 6.5+) and hands the kernel memory it allocated
itself with `zgc_aligned_alloc`. Two consequences:

- The completion queue is ordinary capability-carrying memory, so the poll stays
  a direct load from the safe half — no copy, no syscall, and no uncapabilitied
  pointer anywhere in the design.
- The allocation must be stable, which §2.4 established experimentally.

---

