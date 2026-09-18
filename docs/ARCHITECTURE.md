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

### 4.4 A blocking enter must ask for events

Worth recording because it is a silent trap: `io_uring_enter` ignores
`min_complete` unless **`IORING_ENTER_GETEVENTS`** is set in its flags. With
`min_complete=1` and `flags=0` the kernel processes `to_submit` and returns
immediately, so a "wait" loop spins in userspace at one syscall per iteration
while looking like it sleeps.

It went unnoticed for a long time because the spin almost always resolved before
the park was reached, so the park path was rarely executed. Moving the wait policy
exposed it: the explicit `fasync_result` path was doing **17,233** kernel entries
for 64 reads, and after adding the flag it does **64** -- one real sleep per
request. The same flag was missing from the park path inside the compiler hook.

There is a general lesson in it: a wait loop that "works" is not evidence that it
waits.

### 4.5 Ring memory

Because of §2.3, the runtime does not mmap the ring. It sets
`IORING_SETUP_NO_MMAP` (kernel 6.5+) and hands the kernel memory it allocated
itself with `zgc_aligned_alloc`. Two consequences:

- The completion queue is ordinary capability-carrying memory, so the poll stays
  a direct load from the safe half — no copy, no syscall, and no uncapabilitied
  pointer anywhere in the design.
- The allocation must be stable, which §2.4 established experimentally.

---

## 5. Dependencies

`idea.md` §3 splits the dependent-syscall problem in two, and the implementation
follows that split.

**Explicit dataflow (free).** `x = a(); b(x)` needs no annotation: the pending tag
on `a`'s result *is* the dependency edge, and it is discovered structurally.
`tests/stage2_lazy_resolution.c` exercises the derived-pointer case.

**Descriptors (the free case, extended).** Buffers are not the only thing a
syscall produces. `fasync_open_pending` returns a *pending descriptor* -- negative,
since a real descriptor never is -- and every operation that takes an fd resolves
it first. So `fasync_pread(fd, ...)` on a descriptor that does not exist yet is
what creates the dependency; nothing is declared and nothing is remembered, and
the same dataflow argument that makes buffer provenance free applies. Resolving a
pending descriptor also publishes any queued submissions first, so a program that
never calls `fasync_submit()` explicitly cannot deadlock against its own
unpublished SQEs.

What this gives up is overlap: the read cannot be submitted until the open has
produced an fd. Getting that back means having the kernel chain them, which needs
the open to target an explicit direct-descriptor slot so the read can name it in
advance. `tests/stage6b_fd_chain_probe.c` shows why that is unavailable here:
`IORING_REGISTER_FILES` with a sparse table works, `IOSQE_FIXED_FILE` reads on a
slot work, `IORING_FILE_INDEX_ALLOC` works -- but an openat targeting an *explicit*
slot is not honoured, and the kernel allocates its own index instead. With the
index unknown until completion, a chained read has nothing to name.

**Hidden aliasing (needs a declaration).** `fasync_dep.c` implements declared
effect sets after OpenMP task `depend` clauses and Jade. Each operation declares
what it reads and writes, and the runtime builds the dependency DAG: writer→reader,
reader→writer (anti-dependency), writer→writer. Reader/reader pairs never
conflict.

The Fil-C-specific part is what keeps annotations rare. Before falling back to the
declarations, the analyser asks the runtime for the true extent of the object
behind each pointer (`zgetlower`/`zgetupper` — the InvisiCap bounds) and discharges
any pair whose ranges are provably disjoint:

```
4 operations, 2 dependency edges
  writer-to-reader:  2
conflicts dissolved by proving capability ranges disjoint: 4
-> write(a) and write(b) declare conflicting kinds, but the runtime
   proves their objects disjoint, so they run together instead of
   serializing. That is an annotation nobody wrote.
```

That is the relationship `restrict` has to C's alias analysis, and it is the
concrete payoff of building on a substrate where every pointer carries real
bounds.

### Serialization tokens, measured against effect sets

`async-a-sync.pdf` proposes a different answer to the same problem: rather than
declaring what an operation touches, pass a token to every call that must be
ordered against the others sharing it (a hidden trailing `void* PROVENANCE`
argument in its formulation). It is implemented here as a named resource claimed
`INOUT`, which means it flows through the ordinary analysis instead of needing its
own machinery -- and, usefully, means it composes with effect sets rather than
competing with them.

The same six-operation workload, two independent chains, encoded three ways:

| encoding | edges | peak in flight |
|---|---|---|
| declared effect sets | 4 | 5 |
| two tokens, one per chain | 6 | 2 |
| one token shared by everything | 15 | 1 |

(`peak in flight` is as counted by the scheduler and is indicative rather than an
antichain width; the edge counts are exact.)

**A token is coarser, and the table is the cost.** It can only express a chain:
everyone sharing a token is ordered against everyone else sharing it, including
pairs that do not actually conflict. A single token over this workload imposes a
total order where four operations could have run at once, which is exactly the
over-serialization `idea.md` §3.2 anticipated when it rejected a serial/parallel
split.

Where a token is genuinely the better tool is the case effect sets cannot reach:
a dependency on something with no address -- an fd, a path, a lock. Two calls
colliding on a descriptor have nothing for the capability-range analysis to look
at, and the token expresses it in one line. And because the token is modelled as a
*resource*, the one thing the original formulation cannot do becomes possible:
several operations can share a token as readers (`FASYNC_IN`) at no cost, where a
token that is always a serialization point forces them to queue.

So: worse than effect sets where the dependency is expressible as a range, better
where it is not, and best of all composed -- precise ranges where you have them,
a token for the opaque part.

### The case the design does not get for free

`idea.md` §3.1 flags it precisely: sometimes the dependency is not "code touches
the buffer" but "the *kernel* needs `a`'s bytes as the content of `b`'s SQE". A
`write` whose source buffer is a not-yet-filled async read is exactly that case,
and it cannot be deferred — the kernel reads those bytes when it executes the
request, whatever is there.

The current implementation resolves the source eagerly in `fasync_pwrite`, which
is *correct* but gives up the overlap between the write and the read feeding it.
The kernel-native fix is `IOSQE_IO_LINK`, which chains the two requests so the
write only executes after the read completes. That work is **not done**; see §7.

---

## 6. The compiler patch

`compiler/patches/0001-FilPizlonator-resolve-pending.patch` adds a call to
`filc_resolve_pending` immediately alongside the capability check FilPizlonator
already emits for each access through an escaping pointer.

It is small, and §4.3 is why. Because resolution does not change the pointer's
value, the patch does not rebind pointers, rewrite `Place` projections, or touch
the address computation. It inserts one side-effecting call before the existing
check and changes nothing else:

```cpp
if (PK == PointerKind::Escaping)
  CallInst::Create(
    ResolvePending,
    { flightPtrPtr(FlightPtr, Inst), ConstantInt::get(IntPtrTy, 1) },
    "", Inst)->setDebugLoc(Inst->getDebugLoc());
```

The callee is bound as `filc_resolve_pending`, native and unprefixed, per §2.5.

**It is built and verified.** clang was compiled from these sources (Release,
assertions on, X86 only) and the resulting compiler emits the hook: 5 call sites
appear in a small test program's binary, and `tests/stage4_compiler_hook.c` shows
the hook actually resolving:

```
after submit: returned=1 sqes_queued=1 inflight-still-pending=yes
resolve calls attributed to the access: before=1 after=2
fast-path hits (means 'nothing was in flight'): 4095
hook fired at the access:    yes
completion reaped:           yes
buffer contents correct:     yes
```

Read the two counters together, because they are the whole design in miniature:
one access resolved a pending request, and the other 4095 went through the
one-load fast path. That program contains no explicit resolution call anywhere —
`FASYNC_ACCESS()` is compiled to nothing under
`-DFASYNC_COMPILER_INSERTS_CHECKS` — so the only thing that could have resolved it
is the compiler's own instrumentation.

**Honest note on performance.** This is the simple form: an unconditional call per
escaping-pointer access. The runtime makes it cheap (one load, one predicted
branch, no table touch when nothing is in flight), and restricting it to escaping
pointers keeps it off stack accesses entirely. But the fuller version described in
`idea.md` §2.6 would fold the test into the existing bounds compare — one extra
compare instead of a call — by widening the InvisiCap to carry the pending tag.
That is a better design and it is not what this patch does.

## 7. Known limitations

These are real and are worth stating plainly.

1. **No kernel-native fd chaining.** Descriptor provenance works, but a read on
   a pending descriptor waits for the open rather than being chained after it,
   because the kernel here does not honour an explicit direct-descriptor slot (see
   §5 and `tests/stage6b_fd_chain_probe.c`). Dependent operations are therefore
   ordered in userspace. This is the remaining piece of `idea.md` §3.1.

2. **Detection uses a size of 1.** A derived pointer into a pending buffer always
   starts inside it, so containment of its first byte is what detection needs. A
   pointer that starts *outside* a pending buffer and straddles into it would not
   be detected. That is an unusual access pattern, but it is a gap.

3. **Aliasing writes are not tracked.** Resolution is driven by the checked access
   path. A write to a pending buffer through a path that does not go through it —
   for instance a raw `memcpy` that the pass does not instrument — would race with
   the kernel. §2.4 of `idea.md` anticipated this class of leak.

4. **One ring, one lock.** The runtime keeps a single global ring guarded by
   convention rather than a real lock, and multi-threaded use is not tested. The
   safepoint bracketing on blocking calls is correct for a single thread; a
   multi-threaded collector handshake under concurrent blocking submits has not
   been reasoned through.

5. **`fasync_read` is deliberately absent.** A plain `read` has no offset, so the
   bytes it produces depend on the file cursor at *execution* time, not at
   submission time. That is unresolvable in this model without serializing, so
   only `pread` (offset-carrying, and genuinely parallelizable) is supported.

6. **Supported opcodes are a subset**: `pread`, `pwrite`, `openat`, `close`,
   `fsync`. `read`, `writev`, sockets, and the rest are not wired up.

---

## 8. Measurements, and how to read them

From `demos/demo_async_io.c`, 64 × 256 KiB from a warm page cache:

```
blocking loop:    6.433 ms   (64 separate round trips)
async:            6.441 ms   (0.049 ms to enqueue+publish all 64,
                             then 6.392 ms to resolve)
blocking kernel entries: 1 async vs 64 blocking
observed: 1.00x
```

**The async path is not faster here, and that is the honest reading.** With a warm
page cache the bytes are already in kernel memory, so both paths are memcpy-bound
and the async path's machinery — shared rings, bookkeeping per request, one pass
through the completion queue — is pure overhead. What it removes is the per-request
round trip, and on this workload the round trip was already cheap: it went from 64
entries to 1, and the wall clock did not care.

The compiler-hook test in §6 is the other half of the picture, and it is
qualitative rather than a benchmark: 1 of 4096 accesses paid for a resolution and
the rest cost a load.

What the numbers *do* establish is structural: submission is effectively free
(0.049 ms to enqueue and publish 64 requests), it never blocks (0 blocking entries,
verifiable by counter rather than by timing), and resolution is lazy and cheap
while nothing is in flight. Whether those properties turn into throughput depends
on per-request latency being high enough that having everything in flight at once
matters — cold cache, real devices, network filesystems. **That has not been
measured here, and `idea.md` §6 phase 6 explicitly asks for it** (comparison
against `tokio-uring`/`monoio`). Until that exists, the honest claim is about
mechanism, not about speed.

---

## 9. Layout

```
runtime/
  src/fasync.h             public API (demos link against this)
  src/fasync.c             memory-safe half: rings, request table, ops, provenance
  src/fasync_dep.c/.h      effect sets, disjointness proof, DAG + scheduler
  src/fasync_native.c      trusted half: io_uring syscalls + resolution policy
  src/fasync_io_uring.h    the kernel ABI, shared by both halves
  src/fasync_shared.h      the request/ring layout both halves agree on
  src/fasync_syscalls.h    declarations of the added runtime surface
  patches/                 the libpas forwarder-generator patch
  os-include/              kernel headers for compiling the trusted half
  build.sh                 builds the extension and splices libpizlo.a
compiler/
  patches/                 the FilPizlonator patch
  build.sh                 builds a patched clang (long; see README)
tests/
  run.sh                   builds everything and runs the suite
  stage2b_mmap_probe.c     kernel: which mmaps of a ring are allowed
  stage2c_gc_pin_probe.c   Fil-C: GC memory must not move
  stage2_lazy_resolution.c Fil-C: submission never blocks; resolution is lazy
  stage3_dependency.c      Fil-C: effect sets, disjointness, DAG execution
  stage4_compiler_hook.c   Fil-C: the compiler inserts the hook (needs the
                           patched compiler; skipped when it is not built)
  stage5_trackers.c        Fil-C: serialization tokens vs effect sets, measured
  stage6_fd_provenance.c   Fil-C: operations against a not-yet-open descriptor
  stage6b_fd_chain_probe.c kernel probe: what direct descriptors support here
demos/
  demo_async_io.c          the showcase
docs/ARCHITECTURE.md       this file
vendor/                    Fil-C distribution and source checkout
```

## 10. Building and running

```sh
./runtime/build.sh        # builds the extension into runtime/build/lib/libpizlo.a
./tests/run.sh            # builds and runs the whole suite
./compiler/build.sh       # builds a patched clang (long; see compiler/README.md)

# the showcase, compiled by the patched compiler, with the hook inserted
# automatically and no marker macro in the source:
vendor/fil-c-src/build/bin/filcc -O2 -static -DFASYNC_COMPILER_INSERTS_CHECKS \
  -I runtime/src -L runtime/build/lib -o demo demos/demo_async_io.c
```

`runtime/build.sh` needs the Fil-C 0.685 distribution and a Fil-C source
checkout; both are fetched into `vendor/` (see `README.md`).
