
# Transparent Async Syscalls via Pointer Provenance Tracking

## 1. The core idea

Convert syscalls from synchronous to asynchronous **without changing call-site syntax**. Instead of `await`-ing every I/O operation, the programmer writes ordinary-looking code; asynchrony and parallelism happen underneath, driven by three mechanisms working together:

1. **Zero-context-switch submission.** Every syscall becomes an `io_uring` submission-queue entry (SQE) instead of a synchronous trap. Nothing blocks at the call site.
2. **Provenance-tracked results.** The pointer/value returned by an async syscall is tagged with metadata identifying _which_ pending request it came from. Any pointer derived from it by arithmetic, field access, or indexing inherits that tag — so the whole "family" of derived pointers stays traceable back to the original request.
3. **Lazy resolution on first genuine access.** The moment code actually dereferences a tagged pointer, a cheap check fires: has the completion queue (CQ) entry for that request landed yet? If yes, the check resolves transparently and the access proceeds. If no, the caller blocks (spin, then park) until it has.

The goal: **implicit, syntax-free futures**, with `io_uring`-class throughput underneath, and no `.await` scattered through the program.

---

## 2. How the design evolved over the discussion

### 2.1 First pass — kernel-trap based ("fault into userspace on deref")

Original framing: use page-fault trapping (`mprotect` + `SIGSEGV`, à la `userfaultfd`) so that touching an unresolved pointer transparently traps and the handler resolves it.

**Problem identified:** trapping into the kernel and back on every access to check completion defeats the "no context switch" goal that made `io_uring` attractive in the first place. It also has real engineering costs: an interval tree consulted on every fault, `mprotect`/TLB overhead scaling with in-flight request count, and non-reentrant-signal-handler hazards (allocating/locking inside a `SIGSEGV` handler).

### 2.2 Second pass — pure userspace check, no trap

Revised framing: don't fault into the kernel at all. Instead, **insert an explicit check before the access**, entirely in userspace: consult the completion queue, spin if not ready, resolve and continue.

**Key realization:** since nothing routes control to a userspace checker on a raw CPU load instruction unless something inserts that check first, this requires either:

- **Compiler-inserted read barriers** (exactly how concurrent GC read barriers work — Shenandoah/Azul C4's Brooks forwarding pointers: check a forwarding word before every heap load, redirect and self-heal if the object moved), or
- **Operator-overloaded smart pointers** (`Deref`/`Index`/`Add` in Rust, or C++ operator overloading) that embed the check-then-resolve logic in every access path you control.

The "replace the pointer with the resolved result in place" step also has clean precedent: it's exactly **GHC thunk update / blackholing** in lazy functional runtimes — a thunk overwrites itself with an indirection to its answer on first forcing, and blocks concurrent forcers rather than duplicating work.

### 2.3 Third pass — Rust

- **`Deref` overloading on a smart pointer type** (`AsyncPtr<T>`) gets you the behavior cleanly and idiomatically, with derived-pointer arithmetic captured via `impl Add<usize> for AsyncPtr<T>`.
- **Rust's ownership system helps for a different reason than "provenance tracking" as Rust defines it.** Rust's actual `Provenance`/strict-provenance APIs are a _different_ mechanism — a compile-time/Miri-checked aliasing model for optimizer soundness, not a runtime hook. What actually gives the soundness benefit is that ownership makes it hard for a raw, untracked pointer to escape the wrapper: if you never expose a safe way to extract the naked pointer, every path to the data is forced through the checked `Deref` impl.
- **Escape hatch problem:** any code that reaches through `unsafe`, FFI, or gets the pointer out as a bare `&T`/`*const T` bypasses the check silently. This is a real soundness gap, not just an edge case.

### 2.4 Fourth pass — MIR-level compiler pass

Instead of relying on the programmer to route everything through a wrapper type, insert the barrier automatically via a **custom rustc MIR transform**:

- MIR is the right level (rather than LLVM IR) because Rust has already desugared `*ptr`, field access, and indexing into explicit `Place` projections (`Deref`, `Field`, `Index`) — but type/trait information (which LLVM erases) is still present, so the pass can tell which places are "async-derived."
- Mechanism: tag a marker trait (`AsyncSource`); walk MIR basic blocks; wherever a `Place`'s base local implements `AsyncSource` and is about to be projected through `Deref`, splice in a call to `resolve_or_spin(request_id, offset) -> *mut T` before the real access, rewriting the place to use the resolved pointer.
- **Tooling precedent:** Kani, MIRAI, Prusti, and Will Crichton's `rustc_plugin`/Flowistry work all hook rustc's unstable internal MIR APIs via a custom compiler driver — this is the realistic starting point rather than forking rustc outright.
- **Advantage over trait overloading:** raw `*const T`/`*mut T`/`&T` are builtin types you can't `impl Deref` for — a MIR pass _can_ instrument genuine raw-pointer operations automatically.
- **Cost:** pinned to rustc's internal, unstable, frequently-changing MIR APIs — ongoing maintenance tax, not a one-time build.
- **Same escape hatch, moved:** the instant a tagged pointer crosses FFI, is cast through `usize`, or is erased into `dyn`/generic context the pass can't see through statically, the barrier stops firing.

### 2.5 Fifth pass — is this just async/await?

**Partially, and partially not.** The control-flow _semantics_ (submit → don't block → resolve later) are the same as any future/promise model — that part of the critique is correct. What's genuinely different: async/await moves the waiting point to an explicit syntactic marker (`.await`), compiled into a state machine the programmer must write by hand at each call site. This design moves the waiting point to _any raw memory access_, discovered automatically, with **no `.await` anywhere**.

This is closer to **implicit/transparent futures** — the _original_ future concept (Baker & Hewitt, 1977, MultiLisp), where touching a future looked exactly like touching an ordinary value. Modern async/await was actually a retreat from that idea, specifically because implicit futures make it impossible to tell from reading code whether an operation might block, and are hard to implement efficiently outside a managed runtime. This project is reviving the harder version of the problem async/await was invented to sidestep — using compiler instrumentation (rather than a managed runtime) to make it sound in a systems language.

**What's actually novel, scoped precisely:** not "async syscalls via io_uring" (solved — `tokio-uring`, `monoio`, `glommio`) and not "futures" (solved, decades old) — the novel combination is _implicit_ futures, specifically for syscalls, implemented via compiler-inserted barriers instead of a managed runtime, at `io_uring`-class performance.

### 2.6 Sixth pass — Fil-C as the substrate

Rather than build a MIR pass or a Rust wrapper type, use **Fil-C**'s existing capability infrastructure directly. This was identified as the strongest fit so far, for a specific structural reason: Fil-C already solves the interception problem this project needs.

**What Fil-C already provides:**

- **InvisiCaps**: every pointer carries an _invisible_ capability (bounds + type), stored out-of-band from the pointer's visible bits.
- Fil-C achieves memory safety by transforming _every_ fundamental pointer operation (as seen in LLVM IR) into code that dynamically checks the capability before the access proceeds — via a single LLVM pass, `FilPizlonator`.
- **No escape hatch of any kind** — no `unsafe`, and it's not possible to link to unsafe code outside a small trusted runtime core (`libpizlo`). This directly closes the escape-hatch gap that both the Rust wrapper and Rust MIR approaches had.
- **Syscalls are already a checked boundary**: all buffers passed to system calls are checked for bounds and type, and syscalls specifically route through `libpizlo`, which exposes the low-level syscall surface musl needs.
- Fil-C already uses a **concurrent garbage collector** to atomically invalidate freed pointers — i.e., the runtime already has infrastructure for mutating capability metadata safely out from under a live pointer. This is structurally the same operation your async-resolution step needs.

**Proposed extension:**

- Widen the capability struct to carry a pending-request tag (an `io_uring` request id + byte offset) alongside bounds/type.
- At the point where `FilPizlonator`'s existing check already fires on every load, add: if pending, spin/poll the completion queue, then resolve — either swap the capability to point at the real buffer, or flip the pending bit in place (Brooks-pointer-style self-healing).
- This reuses the check FilPizlonator _already inserts on every access_ rather than adding a second, separate instrumentation pass.

**Modularity of Fil-C, concretely:** it's a fork of LLVM/clang with a real pass-pipeline structure — `FilPizlonator` is one large pass, with a mini pre-pass pipeline before it and the normal LLVM pipeline resumed after it. There's no stable third-party plugin API; the extension point is realistically _inside or immediately adjacent to_ `FilPizlonator` itself, plus a hook in `libpizlo` where syscalls are already recognized as a checked boundary.

**Costs, honestly:**

- Young, effectively single-maintainer project (Filip Pizlo), 64-bit-only currently.
- Already 1.5×–4× slower than legacy C before adding async-resolution overhead — you're building on a substrate still being optimized for baseline performance.
- You'd be patching a moving-target research compiler's core pass, not importing a library.
- A pending capability that never resolves (dropped/cancelled kernel request) is a new use-after-free-shaped failure mode you'd own.

---

## 3. Handling dependent syscalls (`a()` then `b()` depends on `a`)

Two distinct cases, requiring different solutions:

### 3.1 Explicit dataflow dependency — free, no annotation needed

If `b()`'s dependency on `a()` is visible in ordinary compiler dataflow (`x = a(); b(x)`), the compiler already sees the SSA def-use edge. No pragma needed — the pending-capability tag on `a`'s result _is_ the dependency edge, discovered structurally. Both SQEs can be submitted immediately; `b`'s CPU-only prologue runs concurrently with `a`'s I/O, and only the actual access inside `b` to `a`'s data blocks.

- **Where this breaks at the kernel level:** if the dependency isn't just "code touches it" but "the _kernel_ needs `a`'s output as a literal SQE field for `b`" (e.g., `b = write(fd, a_result_ptr, len)`), `io_uring` generally needs concrete, resolved values when it _processes_ an SQE — you can't submit "wait for a future" as a raw field.
- **Partial native solution:** `IOSQE_IO_LINK` sequences SQEs so one runs only after the prior completes; combined with direct/fixed descriptors (`IORING_FILE_INDEX_ALLOC`), a chain like `openat` → linked `read` can reference the not-yet-existent fd via a sentinel slot the kernel fills in at execution time. This is genuine kernel-native promise-chaining, but it cleanly covers the **fd case** specifically — general pointer/buffer dependencies need the destination buffer to already be a real, pre-registered address (`IORING_OP_PROVIDE_BUFFERS` / registered buffers), with only its _contents_ pending — which folds back into the original per-pointer resolution scheme rather than replacing it.
- **Named prior art for the general pattern:** this is **promise pipelining** (Cap'n Proto RPC, the E language's "eventual send") — calling a dependent operation on a not-yet-resolved value without blocking, resolved transparently later.

### 3.2 Hidden aliasing dependency — needs programmer input

If `a()` and `b()` share no explicit value in the source but secretly touch the same memory/fd/path, the compiler cannot see this from dataflow alone. This is the real case requiring an annotation.

**Rejected as too coarse:** a pragma that groups a _set_ of syscalls as "must run synchronously relative to each other" — this over-serializes, since most pairs within a group typically don't actually conflict.

**Proposed instead — per-call declared effect sets**, modeled directly on established prior art:

```c
#pragma filc_syscall depend(out: buf1)
a(buf1);

#pragma filc_syscall depend(in: buf1, out: buf2)
b(buf1, buf2);

c(buf3);  // no declared overlap with a/b -> runs concurrently with both
```

- This is structurally OpenMP's task model (`#pragma omp task depend(in:...) depend(out:...)`), which builds a real dependency DAG from declared read/write sets rather than a binary serial/parallel split.
- Closer, older, and more directly analogous prior art: **Jade** (Stanford, early 1990s) — a language where a `with` block declares which shared objects it will access, and the runtime derives _all_ parallelism and synchronization automatically from those declarations, with no explicit locks or barriers ever written by the programmer. This project's `depend()` pragma is effectively a syscall-scoped reinvention of Jade's access-declaration model.
- **Fil-C-specific refinement that reduces how often the programmer needs to annotate at all:** because every pointer already carries a capability with real bounds, the compiler can attempt automatic disjointness proof between two operands' capability ranges — if provably non-overlapping, no annotation is needed; the `depend()` pragma becomes an escape valve only for cases the analysis can't resolve, the same relationship `restrict` has to alias analysis in C, or `depend` clauses have to OpenMP's own automatic dependency inference.

---

## 4. Related work map (for a related-work section / literature review)

|Concept|Relevance|
|---|---|
|**MultiLisp / Baker & Hewitt futures (1977)**|Original implicit-future model — touching a future looks like touching an ordinary value, no special syntax. The historical ancestor of this whole design.|
|**Async/await (Rust `Future`, JS Promises)**|The "retreat" from implicit futures to explicit, syntactically-marked suspension points, for tractability and readability. This project reopens that trade-off.|
|**Cap'n Proto promise pipelining / E language eventual send**|Named prior art for "call a dependent op on an unresolved future without blocking."|
|**GHC thunks / blackholing**|Precedent for "value resolves once, overwrites itself with the answer, blocks concurrent forcers."|
|**Concurrent GC read barriers (Shenandoah, Azul C4, Brooks forwarding pointers)**|Precedent for "check-before-every-access, self-healing update," inserted by the compiler with no `.await`-style marker.|
|**FlexSC (Soares & Stumm, OSDI 2010)**|"Exception-less syscalls" — batching syscalls to avoid per-call context switches, same goal as `io_uring`'s no-context-switch submission.|
|**io_uring**|Delivers the actual zero-context-switch async I/O substrate; `IOSQE_IO_LINK` + direct descriptors already implement a narrow, kernel-native version of dependency chaining for fds.|
|**AIFM, Fastswap, Infiniswap**|Page-fault-driven transparent remote (RDMA) memory access — the earlier design's "fault on dereference" idea, already implemented for remote memory rather than syscalls.|
|**OCaml 5 effect handlers / algebraic effects**|Alternative paradigm: `perform`/handler composition gets synchronous-looking code with async underneath, arguably the cleanest _general-purpose_ fit for this goal, independent of pointer tricks.|
|**Go goroutines / stackful coroutines / Project Loom**|Alternative paradigm: code that looks fully blocking, resumed on the same stack once the async op completes — sidesteps the provenance problem entirely, since dereference only ever happens post-resumption.|
|**`tokio-uring`, `monoio`, `glommio`**|Existing Rust `io_uring`-backed, thread-per-core async runtimes — solve the performance half of the problem today, without the implicit-future ergonomics.|
|**Fil-C / InvisiCaps**|The proposed implementation substrate: capability-tagged pointers with universal, escape-hatch-free compiler-inserted checks on every access, in unmanaged C/C++.|
|**OpenMP task `depend` clauses**|Direct model for the proposed per-call effect-set annotation and automatic dependency-DAG construction.|
|**Jade (Stanford, early 1990s)**|Closest historical analogue for "declare what a block of code touches; the runtime derives the parallelism," with no explicit synchronization primitives.|

---

## 5. Honest assessment

**Strengths:**

- Genuinely novel combination (implicit futures + `io_uring` performance + escape-hatch-free compiler enforcement), not a rehash of any single piece of prior art.
- Fil-C substrate reuses an existing, already-comprehensive instrumentation pass rather than requiring a new one from scratch.
- The dependent-syscall problem decomposes cleanly into a free case (explicit dataflow) and a genuinely-needs-annotation case (hidden aliasing), with strong, well-tested prior art (OpenMP, Jade) for the latter.

**Risks:**

- Leaky abstraction: any boundary the instrumentation can't see through (FFI, erasure, in Rust's case `unsafe`) silently breaks the illusion rather than failing loudly — worse than async/await's explicit-but-safe failure mode.
- Building on Fil-C means depending on an early-stage, single-maintainer compiler that is itself still chasing baseline performance.
- Kernel-level dependency chaining (Section 3.1's second case) only has clean native support for the fd case; general buffer dependencies fall back to the original per-pointer scheme, not a free kernel-side win.

**Suggested framing for a proposal/thesis:** not "a faster async/await," but _"how far can implicit futures be pushed in an unmanaged systems language using modern compiler capability infrastructure, and is the ergonomic gain worth the residual leaks at unsafe/FFI boundaries."_ That is a legitimate, well-scoped research question with real prior art to build on and a plausible implementation path (Fil-C's `FilPizlonator` extension) rather than a from-scratch instrumentation framework.

---

## 6. Suggested project phases

1. **Prototype the resolution mechanism in isolation.** Build the `Deref`-overloaded `AsyncPtr<T>` in Rust backed by real `io_uring` submissions, to validate the spin/park logic and completion-queue plumbing without touching a compiler.
2. **Survey Fil-C's `FilPizlonator` and `libpizlo` internals** in enough depth to scope the capability-struct extension precisely — this is the highest-uncertainty step and should be timeboxed before committing further.
3. **Extend the InvisiCap format** with the pending-request tag; get a minimal single-syscall (`read`) round-trip working end-to-end.
4. **Implement the explicit-dataflow dependency case** (Section 3.1) — should require no new annotation surface, just correct propagation of the pending tag through derived pointers.
5. **Implement the `depend()` pragma and disjointness-proof fallback** (Section 3.2) for the hidden-aliasing case.
6. **Benchmark against `tokio-uring`/`monoio`** on equivalent workloads to determine whether the ergonomic win justifies the overhead over an explicit-`.await` baseline.
