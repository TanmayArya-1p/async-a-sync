# The FilPizlonator resolution hook

This repo carries a modified copy of the `FilPizlonator` pass
(`compiler/upstream-overrides/FilPizlonator.cpp`, installed over the fetched
vendor source by `compiler/build.sh`) that makes the compiler insert the
async-resolution hook automatically, so programs stop having to mark the points
by hand.

## What it changes

Two places in `llvm/lib/Transforms/Instrumentation/FilPizlonator.cpp`:

1. A `FunctionCallee ResolvePending` member, declared next to the other
   out-of-line runtime entry points (`OptimizedAccessCheckFail` and friends) and
   bound to `filc_resolve_pending` in the callee-initialization block.

2. In `emitChecks`, alongside the capability check the pass already emits for each
   access, a call through that pointer for escaping pointers:

```cpp
if (PK == PointerKind::Escaping)
  CallInst::Create(
    ResolvePending,
    { flightPtrPtr(FlightPtr, Inst), ConstantInt::get(IntPtrTy, 1) },
    "", Inst)->setDebugLoc(Inst->getDebugLoc());
```

That is the whole change - 39 lines, of which most are the comment.

## Why it is this small

Because resolution **flips the pending bit in place** rather than moving the data
(see `docs/ARCHITECTURE.md` §4.3). The kernel was already told where to write, so
the buffer address never changes, so the pointer's value is unchanged, so nothing
downstream needs rebinding. The pass does not have to rewrite `Place` projections
or recompute addresses - it only has to guarantee resolution happened before the
access. The runtime's `filc_resolve_pending` is a side-effecting call that returns
its argument unchanged, and that is enough.

Had resolution instead swapped the capability to point at a different buffer, this
change would have had to plumb a new pointer value into the access, which is a much
larger and riskier change to a pass that is already 17,000 lines long.

## Why it needs a full clang build

`FilPizlonator` is one large pass inside the compiler, and it cannot be added from
outside: the Fil-C distribution ships no `libLLVM` to link an out-of-tree plugin
against (checked - `find build -name 'libLLVM*'` finds nothing), and the
distribution's clang is a single 152 MiB binary.

## Status: built, and verified end to end

The change compiles, and so does the compiler. clang was built from these sources
with `-DCMAKE_BUILD_TYPE=Release -DLLVM_ENABLE_ASSERTIONS=ON -DLLVM_TARGETS_TO_BUILD=X86`
(2772 build steps, about 40 minutes at `-j6` with link jobs serialized), producing
a 167 MiB `clang-20` that reports itself as Fil-C 0.685.

Two levels of verification:

1. **The change compiles.** `ninja .../FilPizlonator.cpp.o` builds clean, with no
   new warnings.

2. **The emitted hook works.** A test program compiled by the patched compiler
   contains 5 call sites to `filc_resolve_pending`, and
   `tests/stage4_compiler_hook.c` shows one of them actually resolving a pending
   request - with the other 4095 accesses taking the one-load fast path. That test
   is built with `-DFASYNC_COMPILER_INSERTS_CHECKS`, so `FASYNC_ACCESS()` compiles
   to nothing and the program contains no explicit resolution call: the resolver
   is the compiler's own instrumentation or nothing.

## The part that was not obvious

Getting from "the pass emits a call" to "the call does something" required an
architectural change, and it is the most interesting thing this exercise turned up.

The obvious target for the emitted call is the memory-safe implementation. That
does not work. A function defined in filcc-compiled code has a `pizlonated_*` entry
point that is an **8-byte descriptor stub**:

```
pizlonated_fasync_resolve_pending:
  lea   0xf5999(%rip),%rax
  mov   %rax,%rdx
  ret
```

Calling it directly returns a function object rather than running the body, and
reaching the body needs the closure/calling-convention protocol the frontend emits.
The symptom was memorable: the compiled binary called the hook on all 4096
accesses, and the implementation's own counters never moved.

A native shim in between does not help either - that just swaps one unsupported
direction for another, since native code cannot simply call into memory-safe code.

What works is what Fil-C already does for its own compiler-emitted calls. Names
beginning with `filc_` are referenced *unprefixed* and called directly, which is
exactly what a pass can emit; every function the pass already calls
(`filc_check_function_call_fail`, `filc_cc_rets_check_failure`, and the rest) is
native. So the resolution **policy** moved into `fasync_native.c`. The memory-safe
half still owns the state and publishes raw addresses into it through
`fasync_shared.h`, which is sound because that memory does not move.

The rule, then: **anything the compiler emits a call to must live in the trusted
runtime.** `docs/ARCHITECTURE.md` §2.5 has the long version.

## Building it

```sh
JOBS=2 ./compiler/build.sh              # conservative; lower JOBS for less RAM
BUILD_TYPE=RelWithDebInfo ./build.sh    # matches Fil-C's own configuration
```

`build.sh` installs the override (idempotently - it copies only when the file
differs from what is already in place), reports free space and job count before
starting, then configures and builds. Expect this to take a long time; see the
comments in the script for why the settings differ from Fil-C's own.

## After it is built

Programs no longer need the marker macro. Build them with
`-DFASYNC_COMPILER_INSERTS_CHECKS` and `FASYNC_ACCESS()` expands to nothing,
because the compiler now emits the real call:

```sh
<build>/bin/filcc -O2 -static -DFASYNC_COMPILER_INSERTS_CHECKS \
  -I runtime/src -L runtime/build/lib -o demo demos/demo_async_io.c
```

The runtime is unchanged either way - which is the point of the split. The
compiler decides *where* resolution is needed; the runtime decides *what*
resolution means. Only the first half is blocked on the rebuild.

## A note on cost

The patched pass emits the call unconditionally for escaping pointers, so it is
one extra call per instrumented access, with the runtime's fast path being a single
load and a predicted branch. Restricting it to escaping pointers keeps it off stack
accesses. The fuller design in `idea.md` §2.6 would widen the InvisiCap to carry
the pending tag and fold the test into the bounds compare the pass already emits -
one extra compare, no call. That is the better end state; this override is the
straightforward one that gets the mechanism working.

The "one extra call" is not free, and it is worth knowing where it bites. The
runtime's fast path is a check that *nothing at all* is in flight, so with a batch
outstanding every instrumented access takes the slow path instead. A 512 KiB scan
through an ordinary function, with three reads still pending, measured 42 ms
against an 8 ms blocking baseline; `docs/ARCHITECTURE.md` §8 has the breakdown,
§7 item 7 states it as a limitation, and `demos/demo_plain_io.c` reproduces it.
That gap is the argument for the compare-instead-of-call end state above.
