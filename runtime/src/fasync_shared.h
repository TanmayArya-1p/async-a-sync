/*
 * fasync_shared.h -- the state the compiler-emitted hook needs, in a form the
 * trusted runtime can reach.
 *
 * WHY THIS EXISTS
 * ---------------
 * The patched FilPizlonator emits a direct call to `filc_resolve_pending` before
 * each access through an escaping pointer. That call has to land on *native*
 * runtime code, for a reason that took a while to pin down:
 *
 *   - Fil-C references names beginning with `filc_` unprefixed and calls them
 *     directly, which is exactly what the pass needs.
 *   - But a function *defined* in filcc-compiled code has a `pizlonated_*` entry
 *     point that is an 8-byte descriptor stub: calling it directly returns a
 *     function object rather than running the body. Reaching the body requires
 *     the closure/calling-convention protocol that the frontend emits.
 *   - So compiler-emitted hooks must be native. That is why Fil-C's own
 *     compiler-emitted runtime calls -- filc_check_function_call_fail,
 *     filc_cc_rets_check_failure, and the rest -- are all implemented natively.
 *
 * Since the hook is native, the resolution logic it needs must be native too, and
 * the state that logic consults has to be described in a layout both halves
 * agree on. That is this header.
 *
 * WHAT CROSSES
 * ------------
 * Only addresses and scalars. The memory itself stays owned by the memory-safe
 * half (the ring is GC memory, the request table is a static array there), and the
 * native half holds raw pointers into it. This is sound because that memory does
 * not move: tests/stage2c_gc_pin_probe.c checks exactly that.
 */

#ifndef FASYNC_SHARED_H
#define FASYNC_SHARED_H

/* Request lifecycle. Must match the enum in fasync.c. */
#define FASYNC_REQ_FREE 0
#define FASYNC_REQ_PENDING 1
#define FASYNC_REQ_DONE 2
#define FASYNC_REQ_FAILED 3

/*
 * One in-flight request, as the native resolver sees it. Must match
 * `struct fasync_req` in fasync.c field for field.
 */
struct fasync_req_shared {
  unsigned long id;     /* handle handed to the caller                    */
  unsigned long gen;    /* generation, so a recycled slot cannot alias    */
  void* buf;            /* result buffer (0 for fd-only operations)       */
  unsigned long len;    /* length of that buffer                          */
  unsigned long offset; /* file offset, or direct-descriptor slot         */
  int fd;               /* target fd                                      */
  unsigned char op;     /* io_uring opcode                                */
  unsigned char state;  /* FASYNC_REQ_*                                   */
  unsigned char linked; /* submitted with IOSQE_IO_LINK                   */
  long result;          /* bytes transferred, or -errno                   */
};

/*
 * Everything the native resolver needs. Every field is a raw pointer into memory
 * the memory-safe half owns.
 */
struct fasync_shared {
  volatile unsigned long* inflight; /* the fast-path gate                 */
  struct fasync_req_shared* reqs;   /* request table                      */
  unsigned long n_reqs;             /* number of slots in that table      */
  int ring_fd;                      /* io_uring ring, for the blocking enter */

  /* Completion ring, so the resolver can poll without a syscall. */
  struct fasync_cqe* cqes;
  unsigned int* cq_head;
  unsigned int* cq_tail;
  unsigned int* cq_mask;
  unsigned int* local_cq_head;

  /* Counters, so native resolution is visible in the same stats as everything
   * else. Pointers rather than values because both halves increment them. */
  unsigned long* userspace_cq_polls;
  unsigned long* resolve_calls;
  unsigned long* fast_path_hits;
  unsigned long* spin_rounds;
  unsigned long* parks;
  unsigned long* kernel_wait_entries;
  unsigned long* completions_reaped;
};

#endif /* FASYNC_SHARED_H */
