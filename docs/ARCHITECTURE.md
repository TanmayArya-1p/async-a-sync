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

