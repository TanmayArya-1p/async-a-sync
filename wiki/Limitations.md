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

This rule is stated in terms of the source function signature. A source
`__int128` may be lowered to multiple machine parameters before the pass sees
it, so rejection and zeroing need compiler tests for that ABI rather than an
assumption about the source type alone.

**Integer dependency keys and width.** Integer keys are zero-extended from
the declared type. A negative value (such as a pending fd handle) declared as
`int` in one function and as `long` in another gives two different keys, and
the calls are not ordered. Declare a shared resource with one integer type.

**Multiple dependency keys.** The framework does not impose a global order on
different keys or detect cycles. Opposite acquisition orders can deadlock: a
source-authorized probe with two tasks holding one key each and requesting the
other timed out repeatedly. Use a consistent application-level order for all
multi-key annotations.

## Pending buffers

**Uninstrumented code.** Only code built by the patched compiler tests the
pending flag. Fil-C's libc is not built that way, so `memcmp`, `strlen`,
`write` and the like read a pending buffer as it stands. Touch the buffer or
wait first.

**Granularity.** A mark covers the whole object. Any access to any part of a
pending object waits, even if the call writes only part of it.

The explicit effect-set range helper currently expands each slice to the
capability object's lower and upper bounds. Disjoint slices of one object
therefore conflict for read/write combinations. This does not match the DAG's
promise that provably disjoint ranges are independent.

**Uncollected results.** Tasks that are never polled or waited on stay
allocated in the arena.

## io_uring runtime

- **Operations.** Only `pread`, `pwrite`, `openat`, `fsync` and `close` are
  supported. `read` and `write` are not, because they have no offset.
- **Capacity.**
  - At most 1024 requests are in flight; further calls wait for a slot.
  - The explicit API has 64 pending-fd handles.
- **Verified capacity issue.** The 64-handle check occurs after the open
  request is queued. A source-authorized real-io_uring probe filled all 64
  handles and issued a 65th; it returned `-1` with `errno == 0` while the
  request count and open descriptors increased.
- **Threads.** One ring and one lock serve all threads, so I/O from many
  threads is serialized. A ring per thread would scale across cores.
- **Where the speedup is.** It is significant only when reads miss the page
  cache.
- **io_uring access.** There is no fallback when io_uring is blocked.

## Maintenance

- **Pinned revision.** The compiler overrides and the SROA patch are tied to
  Fil-C source revision `d80c8bba1c58`. A newer `deluge` may need them
  rebased.
- **Untested targets.** `tests/run.sh` does not run the Makefile demo
  targets. Run them by hand after changing the runtime or the link line.
