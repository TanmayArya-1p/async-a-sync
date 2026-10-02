# Limitations

## Compiler

**Calls that are not rewritten.** The following stay ordinary synchronous
calls:

- calls that expect a non-void scalar return value;
- indirect calls;
- calls through a mismatched prototype.

**Types the body cannot receive.** Floating-point, aggregate and wider-than-64-bit
arguments are not staged, so the body run by the runtime receives 0 for
them.

**Integer dependency keys and width.** Integer keys are zero-extended from
the declared type. A negative value (such as a pending fd handle) declared as
`int` in one function and as `long` in another gives two different keys, and
the calls are not ordered. Declare a shared resource with one integer type.

## Pending buffers

**Uninstrumented code.** Only code built by the patched compiler tests the
pending flag. Fil-C's libc is not built that way, so `memcmp`, `strlen`,
`write` and the like read a pending buffer as it stands, and `memcpy`,
`memset` and the like store into a buffer a queued call is still reading
without waiting for it. Touch the buffer or wait first.

**Reads wait for readers too.** Your code waits for the calls reading a
buffer before it stores into it, so a refill never changes what a queued
pwrite sends. The hook cannot tell a load from a store, so a load waits for
them as well: reading back a buffer you just queued for writing waits for
the write. Read-only objects, such as string literals, are exempt: they
cannot be stored into. A call's own body is exempt too: reading its input,
it waits only for the calls producing it.

**A body reading its own input is slow.** While a call runs, its read mark
keeps its input flagged, so every access its body makes to that input takes
the hook's slow path, without waiting. A body that checksums a 1 MiB input
byte by byte runs about 25x slower than the same loop outside the call.
Copy the input first, or do the reading after the call.

**Hidden writes through `const`.** A body that casts `const` away, or a C++
`mutable` member, writes a buffer the type calls an input. Mark such a
parameter `FILC_BUF`.

**Granularity.** A mark covers the whole object. Any access to any part of a
pending object waits, even if the call writes only part of it.

**Uncollected results.** Tasks that are never polled or waited on stay
allocated in the arena.

## io_uring runtime

- **Operations.** Only `pread`, `pwrite`, `openat`, `fsync` and `close` are
  supported. `read` and `write` are not, because they have no offset.
- **Capacity.**
  - At most 1024 requests are in flight; further calls wait for a slot.
  - The explicit API has 64 pending-fd handles.
- **Threads.** One ring and one lock serve all threads, so I/O from many
  threads is serialized. A ring per thread would scale across cores.
- **Where the speedup is.** It is significant only when reads miss the page
  cache.
- **io_uring access.** There is no fallback when io_uring is blocked.

## Maintenance

- **Pinned revision.** The patches to Fil-C are tied to source revision
  `d80c8bba1c58`. A newer `deluge` may need them rebased.
- **Untested targets.** `tests/run.sh` does not run the Makefile demo
  targets. Run them by hand after changing the runtime or the link line.
