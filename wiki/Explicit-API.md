# Explicit API

**Headers:** [`runtime/src/fasync.h`](../runtime/src/fasync.h),
[`runtime/src/fasync_dep.h`](../runtime/src/fasync_dep.h)
**Library:** `libfilc_async_uring.a`

The explicit API issues io_uring requests by hand, with no annotation. It is
part of the io_uring runtime, not the generic framework. It shares the
runtime's ring, request table and lock, so explicit and annotated requests
can be mixed.

Every function may be called from any thread. The lock adds safety, not
parallelism.

The request functions are protected by the ring lock, but an effect-set token
(`fasync_tracker`) is not internally synchronized. Concurrent conflicting
tagged calls using one tracker can overlap before either request is recorded.
Serialize tagged calls per tracker until the token implementation provides
its own synchronization.

## Requests

```c
typedef unsigned long fasync_id;   /* one in-flight request; 0 on failure (errno set) */

fasync_id fasync_pread (int fd, void* buf, size_t len, unsigned long offset);
fasync_id fasync_pwrite(int fd, void* buf, size_t len, unsigned long offset);
fasync_id fasync_openat(int dirfd, const char* path, int flags, int mode);
fasync_id fasync_fsync (int fd);
fasync_id fasync_close (int fd);
```

**Queueing.** Each call queues one request and returns without entering the
kernel.

**`fasync_pread`.** Its buffer is marked pending through the framework. With
the patched compiler, the first access to it waits for the read, exactly like
an annotated call. Several reads may target one object.

**`fasync_pwrite`.** Waits first for any call still producing its source
buffer.

**Completion.**

| Function | Behavior |
|---|---|
| `fasync_submit()` | Sends every queued request to the kernel in one entry. Returns the number sent, or a negative value on error. |
| `fasync_ready(id)` | Nonzero once the request has completed. Reaps completions without blocking. |
| `fasync_result(id)` | Waits for the request and returns its result (bytes, fd, 0, or `-errno`). Frees the request slot, so call it once per id. |
| `fasync_wait_all()` | Sends the queue and waits for every request in flight. 0 on success, -1 on timeout. |

## Pending file descriptors

```c
int  fasync_open_pending(int dirfd, const char* path, int flags, int mode);
long fasync_fd_resolve(int fd);
```

**`fasync_open_pending`** queues an open and returns a negative handle (-2,
-3, …). You can use that handle wherever an fd is accepted. A request issued on it
first waits for the open, then uses the real fd.

**`fasync_fd_resolve`** returns the real fd, waiting for the open if needed.

**Limit.** At most 64 pending handles exist at once.

The current implementation checks this limit after queuing the open. When all
64 slots are occupied, the 65th call returns `-1` with `errno == 0` while its
request remains queued. Treat this as an implementation defect; do not assume
that `-1` means no request was queued.

## Resolving from uninstrumented code

```c
void* fasync_resolve_pending(void* ptr, size_t size);
#define FASYNC_ACCESS(ptr, size)   /* no-op with -DFASYNC_COMPILER_INSERTS_CHECKS */
```

`fasync_resolve_pending` waits until no call owns the object `ptr` points
into, then returns `ptr`. Use it, or `FASYNC_ACCESS`, before code the patched
compiler did not build reads a pending buffer.

## Provenance

```c
struct fasync_prov { fasync_id req; unsigned long offset; unsigned long len; };
int   fasync_provenance(const void* ptr, size_t size, struct fasync_prov* out);
void* fasync_derive(void* base, unsigned long offset, size_t len, struct fasync_prov* out);
```

**`fasync_provenance`** reports which in-flight request covers
`[ptr, ptr+size)`, and at what offset into its buffer. It returns 0 if no
request covers the range.

**`fasync_derive`** returns `base + offset` and fills `out` in the same way.

## Effect sets and tokens (`fasync_dep.h`)

For ordering explicit requests, there are two mechanisms:

- **An effect-set DAG.** Describe each operation's accesses:
  - `FASYNC_ACCESS_RANGE(ptr, len, kind)` for a byte range;
  - `FASYNC_ACCESS_RESOURCE(id, kind)` for a named resource, where `kind` is
    `FASYNC_IN`, `FASYNC_OUT` or `FASYNC_INOUT`.

  `fasync_build_dag` computes the conflict edges, dissolving pairs whose byte
  ranges are provably disjoint. `fasync_run_dag` issues the operations in
  waves that respect the edges.
- **Tokens.** `fasync_tracker_new()` creates a token.
  `fasync_tagged_pread`/`fasync_tagged_pwrite` wait for earlier tagged calls
  on the same token that conflict with their access kind.

The token tracker is not safe for concurrent updates by multiple callers.

This is separate from the `r_dep=`/`w_dep=` locks of annotated calls. See
`demos/demo_async_io.c` and `demos/demo_provenance.c`.

The effect-set range helper currently expands slices to the capability
object's bounds. Disjoint slices of one object can therefore conflict, even
though the DAG description says disjoint ranges are independent.

## Statistics

```c
struct fasync_stats {
    unsigned long sqes_queued;
    unsigned long kernel_submit_entries;
    unsigned long kernel_wait_entries;   /* context switches actually taken */
    unsigned long userspace_cq_polls;
    unsigned long parks;
    unsigned long completions_reaped;
};
void fasync_get_stats(struct fasync_stats* out);
void fasync_reset_stats(void);
const char* fasync_last_error(void);
```

These count ring activity for both explicit and annotated requests.
