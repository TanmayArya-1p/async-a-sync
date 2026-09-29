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
| Undefined `pizlonated_pthread_mutex_*` | The trailing `-lc` is missing. End the link line with `-lpizlo -lfilc_async_uring -lpizlo -lc`. |
| Undefined `pizlonated_filc_async_submit` or `pizlonated_filc_async_runtime_poll` | No runtime is linked. Add `-lfilc_async_uring`, or your runtime's objects. |
| Undefined `pizlonated_filc_async_complete` or other framework symbols from the runtime | `-lpizlo` appears only before the runtime. Name it on both sides. |

## Compile

| Symptom | Cause and fix |
|---|---|
| `FilAsync: malformed filc_async option` | An option is invalid. The line above it names the option. See [Errors](Annotation-Reference.md#errors). |
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

## Tests

| Symptom | Cause and fix |
|---|---|
| Many tests fail at request creation | io_uring is blocked in this environment. This is not a dispatch bug. |
| Tests reported as skipped | The patched compiler is missing, or io_uring is unavailable. The run prints which. |
