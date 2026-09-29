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
`write` and the like read a pending buffer as it stands. Touch the buffer or
wait first.

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

- **Pinned revision.** The compiler overrides and patches are tied to
  Fil-C source revision `d80c8bba1c58`. A newer `deluge` may need them
  rebased.
- **Untested targets.** `tests/run.sh` does not run the Makefile demo
  targets. Run them by hand after changing the runtime or the link line.
