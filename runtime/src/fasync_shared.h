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

#include <stddef.h>

/* Request lifecycle. Must match the enum in fasync.c. */
#define FASYNC_REQ_FREE 0
#define FASYNC_REQ_PENDING 1
#define FASYNC_REQ_DONE 2
#define FASYNC_REQ_FAILED 3

/*
 * Slots in the request table.
 *
 * Part of the shared contract rather than a private detail of fasync.c, because
 * the native resolver walks this table and the allocation bitmap below is sized
 * from it.
 *
 * Sized so a whole workload fits in flight at once: the point of the design is
 * that a program with a few hundred independent operations submits all of them
 * before consuming any, and a table smaller than the workload forces it into
 * waves, which is exactly the serialisation it is trying to remove. It costs a
 * 40 KB table and 16 bitmap words.
 */
#define FASYNC_MAX_INFLIGHT 1024

/* One bit per request slot, set while that slot is allocated. */
#define FASYNC_ALLOC_WORDS (FASYNC_MAX_INFLIGHT / 64)

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

  /*
   * Which slots are worth looking at.
   *
   * The resolver's job on the slow path is "is this address inside a pending
   * result buffer?", which is a walk of the request table. Walking all 256 slots
   * on an access that is inside none of them is the expensive case, and it is
   * the common one: a program scanning a buffer pays it once per byte. Most
   * slots are free, so a word-at-a-time bitmap lets the walk skip 64 slots per
   * load and only touch the handful that are actually allocated.
   */
  unsigned long alloc_bits[FASYNC_ALLOC_WORDS];

  /*
   * The allocation epoch, bumped by the memory-safe half every time a request is
   * allocated. It exists to invalidate the memo below.
   */
  unsigned long alloc_epoch;

  /*
   * A range known to contain no pending result buffer.
   *
   * The slow path exists to answer "is this address inside a buffer somebody is
   * still filling?", and for a program scanning a buffer that is not itself
   * pending the answer is "no" once per byte. Proving that by walking the table
   * costs more than the scan does (demos/demo_plain_io.c measures it), so a miss
   * is remembered: start/end is a range proved empty of pending buffers, and
   * `epoch` is the allocation epoch it was proved at.
   *
   * Any allocation invalidates it, because a freed buffer's address can be handed
   * out again. Completions do not: a request that finishes only ever shrinks
   * pending coverage. `end == 0` means no upper bound, which is the usual case --
   * any buffer at all would have to start above the scan to bound it.
   */
  struct fasync_memo {
    unsigned long epoch;
    const char* start;
    const char* end;
  } memo;

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
  unsigned long* memo_hits;
};

/* Has the memo already proved that nothing pending covers [p, p + size)? */
static inline int fasync_memo_covers(const struct fasync_shared* sh, const char* p,
                                     size_t size) {
  if (sh->memo.epoch != sh->alloc_epoch || !sh->memo.start)
    return 0;
  if (p < sh->memo.start)
    return 0;
  if (sh->memo.end && p + size > sh->memo.end)
    return 0;
  return 1;
}

/*
 * The pending request covering [ptr, ptr+size), or 0 if there is none.
 *
 * ONE implementation, used by both halves. The two used to carry a copy each --
 * they are compiled by different toolchains and only share this header -- which
 * meant the memo below would have had to be written twice and kept in step by
 * hand. It is a walk of a table and a comparison of addresses, so it belongs in
 * the header where both halves see the same code.
 *
 * A completed request is skipped, which is the self-healing step: once a request
 * resolves, its range stops being reported as pending.
 */
static inline struct fasync_req_shared* fasync_shared_find(struct fasync_shared* sh,
                                                           const void* ptr,
                                                           size_t size) {
  const char* p = (const char*)ptr;
  if (fasync_memo_covers(sh, p, size)) {
    (*sh->memo_hits)++;
    return 0;
  }

  const char* limit = 0; /* nearest pending buffer that starts above p */
  struct fasync_req_shared* found = 0;

  for (unsigned long w = 0; w < FASYNC_ALLOC_WORDS; w++) {
    unsigned long bits = sh->alloc_bits[w];
    if (!bits)
      continue;
    for (unsigned long b = 0; b < 64; b++) {
      if (!(bits & (1UL << b)))
        continue;
      unsigned long i = w * 64 + b;
      if (i >= sh->n_reqs)
        break;
      struct fasync_req_shared* r = &sh->reqs[i];
      if (r->state != FASYNC_REQ_PENDING || !r->buf)
        continue;
      const char* start = (const char*)r->buf;
      const char* end = start + r->len;
      if (p >= start && p + size <= end) {
        found = r;
        break;
      }
      if (start > p && (!limit || start < limit))
        limit = start;
    }
    if (found)
      break;
  }

  if (found)
    return found;

  /*
   * Nothing covers this address, and `limit` is the nearest buffer above it. No
   * pending buffer can cover anything in [p, limit): one either starts at or
   * above limit, or ends at or below p, and the latter cannot reach past p. So
   * the range can be remembered, and a sequential scan through a buffer that is
   * not pending hits this memo for every remaining byte.
   */
  sh->memo.epoch = sh->alloc_epoch;
  sh->memo.start = p;
  sh->memo.end = limit;
  return 0;
}

#endif /* FASYNC_SHARED_H */
