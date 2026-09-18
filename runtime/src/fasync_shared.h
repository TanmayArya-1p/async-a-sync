/*
 * fasync_shared.h -- the state the compiler-emitted hook needs, in a form the
 * trusted runtime can reach.
 *
 * The patched FilPizlonator emits a direct call to `filc_resolve_pending`
 * before each access through an escaping pointer. That call must land on
 * *native* runtime code: a function defined in filcc-compiled code has a
 * `pizlonated_*` entry point that is an 8-byte descriptor stub, so calling it
 * directly returns a function object instead of running the body. Fil-C's own
 * compiler-emitted calls (filc_check_function_call_fail and friends) are all
 * native for the same reason. So the resolution policy is native, and the state
 * it consults is described here in a layout both halves agree on.
 *
 * Only addresses and scalars cross this boundary. The memory itself stays owned
 * by the memory-safe half (the ring is GC memory, the request table a static
 * array there), and the native half holds raw pointers into it. That is sound
 * because that memory does not move: stage2c_gc_pin_probe.c checks it.
 */
#pragma once

#include <stddef.h>

/* Request lifecycle. */
#define FASYNC_REQ_FREE 0
#define FASYNC_REQ_PENDING 1
#define FASYNC_REQ_DONE 2
#define FASYNC_REQ_FAILED 3

/* Request table size (the native resolver walks it, and the bitmap below is
 * sized from it). Sized so a whole workload fits in flight at once: a table
 * smaller than the workload would force it into waves, which is exactly the
 * serialisation the design removes. Costs a 40 KB table and 16 bitmap words. */
#define FASYNC_MAX_INFLIGHT 1024

/* One bit per request slot, set while that slot is allocated. */
#define FASYNC_ALLOC_WORDS (FASYNC_MAX_INFLIGHT / 64)

/* One in-flight request, as the native resolver sees it. Must match the layout
 * the memory-safe half initializes in fasync.c. */
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

/* Everything the native resolver needs. Every field is a raw pointer into
 * memory the memory-safe half owns. */
struct fasync_shared {
  volatile unsigned long* inflight; /* the fast-path gate                 */
  struct fasync_req_shared* reqs;   /* request table                      */
  unsigned long n_reqs;             /* number of slots in that table      */
  int ring_fd;                      /* io_uring ring, for the blocking enter */

  /* Which slots are worth looking at: the slow-path answer "is this address
   * inside a pending result buffer?" is a walk of the table, and most slots are
   * free, so a word-at-a-time bitmap lets the walk skip 64 slots per load. */
  unsigned long alloc_bits[FASYNC_ALLOC_WORDS];

  /* Allocation epoch, bumped every time a request is allocated. Invalidates the
   * memo below: a freed buffer's address can be handed out again. */
  unsigned long alloc_epoch;

  /* A range known to contain no pending result buffer. A miss in the table walk
   * is remembered so a sequential scan through a non-pending buffer costs one
   * walk, not one per byte. `epoch` is the allocation epoch it was proved at;
   * `end == 0` means no upper bound (any buffer would start above the scan). */
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

  /* Submission-ring state, for the lazy batch auto-submit: nothing forces the
   * caller to submit; the first resolution that genuinely needs a completion
   * publishes the whole queued batch in one non-blocking enter. */
  unsigned int* sq_tail;  /* kernel's SQ tail word, written to publish       */
  unsigned int* sq_mask;  /* ring size mask                                  */
  unsigned int* sq_array; /* the SQ index array                              */
  unsigned int* sqe_head; /* next SQE slot to publish (safe-side value)      */
  unsigned int* sqe_tail; /* next SQE slot to write (safe-side value)        */
  unsigned int* queued;   /* SQEs written but not yet published              */

  /* Counters, so native resolution is visible in the same stats as everything
   * else. Pointers rather than values because both halves increment them. */
  unsigned long* userspace_cq_polls;
  unsigned long* resolve_calls;
  unsigned long* fast_path_hits;
  unsigned long* spin_rounds;
  unsigned long* parks;
  unsigned long* kernel_wait_entries;
  unsigned long* kernel_submit_entries; /* non-blocking publishes */
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

/* The pending request covering [ptr, ptr+size), or 0 if there is none.
 *
 * ONE implementation, used by both halves: they are compiled by different
 * toolchains and used to carry a copy each, which meant the memo had to be kept
 * in step by hand. It is a walk of a table and some address comparisons, so it
 * belongs where both sees the same code. A completed request is skipped, which
 * is the self-healing step: once resolved, a range stops reporting as pending.
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

  /* Nothing covers this address, and `limit` is the nearest buffer above it, so
   * nothing pending can cover [p, limit): one either starts at or above limit,
   * or ends at or below p. A sequential scan through a non-pending buffer hits
   * this memo for every remaining byte. */
  sh->memo.epoch = sh->alloc_epoch;
  sh->memo.start = p;
  sh->memo.end = limit;
  return 0;
}