# Troubleshooting

## Build

| Symptom | Cause and fix |
|---|---|
| Missing `crtbegin.o`, `filc_crt.o`, or `-lyolort` | The patched driver cannot find `pizfix`. Check that `vendor/fil-c-src/pizfix` links to the distribution's `pizfix` (see [Build and link](Building-and-Linking.md#build-the-compiler)). |
| `SROA.cpp`: `AI` undeclared in a Release build | The SROA patch was not applied. Rerun `compiler/build.sh` on the source checkout. |
| A compiler process is killed during the Clang build | Out of memory. Lower `JOBS`; Ninja keeps finished objects. |
| Override or patch fails to apply | The source checkout is not at the pinned revision `d80c8bba1c58`. |

## Link

| Symptom | Cause and fix |
|---|---|
| Undefined `pizlonated_pthread_mutex_*` | The trailing `-lc` is missing. End the link line with `-lfilc_async_uring -lpizlo -lc`. |
| Undefined `pizlonated_filc_async_runtime_<name>` | A function names `runtime=<name>`, but that runtime is not linked. Add it (`-lfilc_async_uring` for `io_uring`), or fix the name. |
| Undefined `pizlonated_filc_async_complete` or other framework symbols from a runtime | The runtime comes after `-lpizlo`. Put every runtime before it. |
| Multiple definitions of a runtime's functions when linking two runtimes | A runtime exports more than its descriptor. Make its functions and state `static`. |

## Compile

| Symptom | Cause and fix |
|---|---|
| `FilAsync: malformed filc_async option` | An option is invalid. The line above it names the option. See [Errors](Annotation-Reference.md#errors). |
| `FilAsync: <f> names no runtime; add runtime=<name>` | Every annotation needs `runtime=`, for example `"runtime=io_uring"`. |
| `FilAsync: call to <f> returns i32 but the stub returns ptr; call left in place` | The annotated function returns a scalar. Make it return `void*` or `void`. |
| `FilAsync: call to <f> is not a plain call matching its prototype` | The call goes through a different prototype or is an `invoke`. It stays a synchronous call. |
| Warning (or error with `-Werror=pragma-clang-attribute`) about an unused attribute | The pragma surrounds a call, not a function declaration. Move it to the declaration. |

## Run

| Symptom | Cause and fix |
|---|---|
| `filc_async: fatal: function cannot be registered on this runtime` before `main` | The runtime rejected an annotated function. Its `op=` is unknown, or its argument kinds do not fit the op (see [supported operations](io_uring-Runtime.md#supported-operations)). |
| `io_uring_setup: Operation not permitted` | The environment blocks io_uring. In Docker, use `--security-opt seccomp=unconfined`. Rosetta does not support io_uring at all. |
| An annotated read returns `-75` (`EOVERFLOW`) without reaching the kernel | Staged arguments were read with an 8-byte stride. Each cell is 16 bytes (see [Staged arguments](Runtime-API.md#staged-arguments)). |
| A buffer passed to `memcmp`/`strlen`/`write` holds stale data | Fil-C's libc is not instrumented. Touch the buffer, wait on the call, or call `fasync_resolve_pending` first. |
| The body of an annotated function never runs | The call was not rewritten (see Compile above), or your runtime does not run bodies. |
| A program hangs with a custom runtime | The runtime does not make progress when polled with `FILC_ASYNC_POLL_BLOCK`, or holds a lock across `filc_async_run`. See [Rules for runtimes](Runtime-API.md#rules-for-runtimes). |
| `filc safety error: cannot read pointer with null object` in `filc_async_validate_table`, before `main` | Known defect on `c070bda`. The pass never emits a reference to `filc_async_runtime_<name>`, so `meta->runtime` is statically zero. See [below](#annotated-calls-panic-before-main-runtime-descriptor-never-referenced). |

## Tests

| Symptom | Cause and fix |
|---|---|
| Many tests fail at request creation | io_uring is blocked in this environment. This is not a dispatch bug. |
| Tests reported as skipped | The patched compiler is missing, or io_uring is unavailable. The run prints which. |

## Annotated calls panic before `main`: runtime descriptor never referenced

Every annotated program on `c070bda` dies before `main`:

```
filc safety error: cannot read pointer with null object.  pointer: 0x20000001c
semantic origin: runtime/src/filc_async.c:805:62: filc_async_validate_table
    <somewhere>: __filc_async_ctor
```

`make demo-pragma-hello` reproduces it, so no pragma demo runs.

### Symptom

`filc_async_validate_table` reads `m->runtime` (line 805) to call
`m->runtime->validate(m)`. Line 803 guards it with `if (!m->runtime)`, but
that guard does not fire: under Fil-C the field holds a *tagged* null, not a
zero word, so the test is false and the dereference proceeds and traps. The
zero capability is also why the "function names no runtime" message never
appears.

### Cause

`FilAsync.cpp` asks for the runtime descriptor with:

```cpp
Constant *Runtime = M.getOrInsertGlobal(
    ("filc_async_runtime_" + parseRuntime(OrigName, Info)).str(), PtrTy);
```

`getOrInsertGlobal` with no initializer and no linkage creates a *tentative
definition* — a global this module owns — not a declaration of the symbol the
runtime library exports. Nothing in the demo's object file refers to
`filc_async_runtime_io_uring`, so nothing links against the definition in
`libfilc_async_uring.a`, and the field stays statically zero.

```
$ readelf -sW hello.o | grep -i runtime_io_uring      # no output
$ strings hello.o | grep runtime=                      # runtime=io_uring
$ nm runtime/build/lib/libfilc_async_uring.a | grep ' T .*runtime_io_uring'
0000000000000000 T pizlonated_filc_async_runtime_io_uring
```

A C `extern` declaration and a tentative definition behave differently, which
is the whole bug:

| Declaration in the module | Emitted symbol |
|---|---|
| `extern void* ext;` | `UND pizlonated_ext_runtime` — a real reference |
| `void* tent;` | `LOCAL OBJECT pizlonatedDO_tent_runtime` — a local zeroed object, no relocation |

Two consequences beyond the panic:

- The documented link-time safety net does not work. Both
  [Annotation-Reference](Annotation-Reference.md) and
  [Writing-a-Runtime](Writing-a-Runtime.md) promise an undefined
  `pizlonated_filc_async_runtime_<name>` when a named runtime is not linked. No
  such error is produced, so a missing runtime is discovered as a null
  dereference at startup instead.
- The failure is silent at compile and link time, which is why a green build
  does not imply a working program.

### Fix

Emit a declaration with explicit external linkage so the reference survives:

```cpp
Constant *Runtime = new GlobalVariable(
    M, PtrTy, /*isConstant=*/true, GlobalValue::ExternalLinkage,
    /*Initializer=*/nullptr,
    ("filc_async_runtime_" + parseRuntime(OrigName, Info)).str());
```

This is the one-line change; it also restores the intended link error for an
unlinked runtime. Two things are worth checking alongside it:

- Line 803's guard should test the capability properly rather than relying on
  the field being a zero word, so a null descriptor reports the intended
  message instead of trapping.
- `validate_table` may be indexing past the end of `metas` if the pass's
  per-TU table is terminated differently. Not the cause here, but it is the
  next thing to read if the panic survives the fix.

