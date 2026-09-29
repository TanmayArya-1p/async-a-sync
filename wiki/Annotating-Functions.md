# Annotate a function

This guide shows how to turn a C function into an asynchronous call, and how
to declare its buffers and its dependencies on other calls. For the exact
meaning of every option, see the [annotation reference](Annotation-Reference.md).

## Mark the function

Wrap the declaration or definition in a `filc_async` annotation pragma:

```c
#pragma clang attribute push(__attribute__((annotate("filc_async", "runtime=io_uring", "op=pread", "bout=buf"))), apply_to=function)
void* read_at(int fd, void* buf, size_t len, unsigned long offset);
#pragma clang attribute pop
```

- **`runtime=`.** Names the runtime that runs the call. It is required, and
  the program must link that runtime: `runtime=io_uring` needs
  `-lfilc_async_uring`. Different functions in one program may name
  different runtimes.
- **Return type.** Must be a pointer or `void`. A call returns the call's task
  handle. The compiler leaves a call that expects a scalar return as a direct
  call and prints a diagnostic.
- **`op=`.** Names the operation. The function's runtime decides which ops exist.
  The io_uring runtime supports `pread`, `pwrite`, `openat`, `fsync` and `close`
  (see [its reference](io_uring-Runtime.md#supported-operations)). An op the
  runtime does not support aborts the program at startup.
- **Arguments.** Options name parameters (`bout=buf`), never positions. Give
  the parameters names in the annotated declaration, or in the definition.
- **Warnings.** Always compile with `-Werror=pragma-clang-attribute`. A pragma
  placed around a call site instead of a function is otherwise only a warning,
  and it does nothing.

## Annotate a function declared in a header

Put the pragma in the header, around the declaration. Every file that
includes it gets its calls rewritten. The definition can be:

- **In another file.** Its ordinary symbol is kept, so the runtime can call it.
- **In the same file, annotated or not.** If both the declaration and the
  definition carry the annotation, the definition's options apply.

The body is the function's own code. The io_uring runtime runs it once for
each call, before issuing the request, so you can log or instrument calls
there. Another runtime might run it on a worker thread, or not at all.

## Declare buffers

Pointer arguments are the buffers a call may touch. Tell the framework which
way each buffer flows:

| Option | Meaning | Marked pending? |
|---|---|---|
| `bout=<param>` | the call writes `<param>` | yes, until the call completes |
| `bin=<param>` | the call only reads `<param>` | no |
| `buf=<param>` | direction unknown | yes |
| (no option) | an unannotated pointer argument is treated like `buf=` | yes |

A pending buffer blocks the first access to it until the call completes. Mark
inputs with `bin=` so reading them does not wait. `bin=`, `bout=` and `buf=`
must name pointer arguments. Other arguments need no option: the runtime
knows its own ops' signatures, such as the io_uring runtime's descriptor in
argument 0.

## Order calls that share a resource

By default, independent calls are all in flight at once. When calls share
something the buffers do not show, such as a file descriptor, list what each
call reads and writes:

```c
#pragma clang attribute push(__attribute__((annotate("filc_async", "runtime=io_uring", "op=pwrite", "bin=buf", "w_dep=fd:file"))), apply_to=function)
void* write_at(int fd, const void* buf, size_t len, unsigned long offset);
#pragma clang attribute pop

#pragma clang attribute push(__attribute__((annotate("filc_async", "runtime=io_uring", "op=pread", "bout=buf", "r_dep=fd:file"))), apply_to=function)
void* read_at(int fd, void* buf, size_t len, unsigned long offset);
#pragma clang attribute pop
```

- **`r_dep=<param>:<namespace>`.** The call reads the resource `<param>`
  names, in `<namespace>`.
- **`w_dep=<param>:<namespace>`.** The call writes it.
- **Keys.** A call's key is the argument's value together with the parameter
  name and the namespace. An integer argument counts by its value, a pointer
  argument by the object it points into.
- **How calls are ordered.** Two calls whose keys match run in call order if
  either one writes, so the `read_at` above waits for an earlier `write_at` on
  the same fd. Two reads can overlap.

### Name a resource the same way everywhere

Both functions above call their descriptor `fd` and use the namespace
`file`, so their keys match. A function that calls it `handle`, or that
uses `w_dep=fd:meta`, locks a different resource, and runs independently of
them. That is how you keep apart things that share a value: fd 3 as "the
file's data" (`fd:file`) and fd 3 as "the file's metadata" (`fd:meta`).

The namespace is required, so every dependency says what it protects. If you
list two dependency options on one parameter, they must agree in mode and
namespace.

## Get the result

The call returns a task handle. You only need it for the result: a byte count,
fd, zero, or `-errno` for the io_uring runtime.

```c
struct filc_async_result_s r = { .pending = read_at(fd, buf, len, 0) };
filc_async_wait(&r);            // or: while (!filc_async_poll(&r)) do_other_work();
if (r.state == 2)               // 0 done, 1 pending, 2 failed
    fprintf(stderr, "read failed: %ld\n", r.result);
```

You never need to wait before touching a `bout=` buffer: the first access
waits for you.

## Touch buffers from code the compiler did not build

Only code built by the patched compiler tests the pending flag. Fil-C's libc
(`memcmp`, `strlen`, `write`, …) reads a pending buffer as it stands. Before
passing a pending buffer to such code, do one of the following:

- touch it first, for example with `(void)*(volatile char*)buf;`;
- wait on the call;
- with the io_uring runtime linked, call `fasync_resolve_pending(buf, len)`.

## Check what the compiler did

Build with `-mllvm -filc-async-debug` to print each function the pass
enrolled, with its options. [Troubleshooting](Troubleshooting.md) lists the pass's
error messages.
