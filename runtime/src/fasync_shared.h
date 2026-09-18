#pragma once

#include <stddef.h>

#define FASYNC_REQ_FREE 0
#define FASYNC_REQ_PENDING 1
#define FASYNC_REQ_DONE 2
#define FASYNC_REQ_FAILED 3

/* sized so a whole workload fits in flight */
#define FASYNC_MAX_INFLIGHT 1024

#define FASYNC_ALLOC_WORDS (FASYNC_MAX_INFLIGHT / 64)

/* one in-flight request as the native resolver sees it */
struct fasync_req_shared {
  unsigned long id;     /* handle handed to the caller */
  unsigned long gen;    /* so a recycled slot cannot alias */
  void* buf;            /* result buffer zero for fd only ops */
  unsigned long len;
  unsigned long offset;
  int fd;
  unsigned char op;
  unsigned char state;  /* FASYNC_REQ_* */
  unsigned char linked; /* submitted with io_link */
  long result;          /* bytes transferred or -errno */
};

struct fasync_shared {
  volatile unsigned long* inflight; /* the fast-path gate */
  struct fasync_req_shared* reqs;
  unsigned long n_reqs;
  int ring_fd; /* for the blocking enter */

  /* one bit per slot so free slots skipped 64 at a time */
  unsigned long alloc_bits[FASYNC_ALLOC_WORDS];

  /* bumped on every allocation to invalidate the memo */
  unsigned long alloc_epoch;

  /* range known to contain no pending result buffer */
  struct fasync_memo {
    unsigned long epoch;
    const char* start;
    const char* end;
  } memo;

  struct fasync_cqe* cqes;
  unsigned int* cq_head;
  unsigned int* cq_tail;
  unsigned int* cq_mask;
  unsigned int* local_cq_head;

  /* submission ring state for the lazy batch auto-submit */
  unsigned int* sq_tail;  /* kernel's sq tail word */
  unsigned int* sq_mask;
  unsigned int* sq_array;
  unsigned int* sqe_head; /* next sqe slot to publish */
  unsigned int* sqe_tail; /* next sqe slot to write */
  unsigned int* queued;   /* sqes written but not yet published */

  /* counters incremented from both halves */
  unsigned long* userspace_cq_polls;
  unsigned long* resolve_calls;
  unsigned long* fast_path_hits;
  unsigned long* spin_rounds;
  unsigned long* parks;
  unsigned long* kernel_wait_entries;
  unsigned long* kernel_submit_entries;
  unsigned long* completions_reaped;
  unsigned long* memo_hits;
};

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

/* the pending request covering this range or zero */
static inline struct fasync_req_shared* fasync_shared_find(struct fasync_shared* sh,
                                                           const void* ptr,
                                                           size_t size) {
  const char* p = (const char*)ptr;
  if (fasync_memo_covers(sh, p, size)) {
    (*sh->memo_hits)++;
    return 0;
  }

  const char* limit = 0; /* nearest pending buffer above p */
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

  /* nothing pending up to limit so remember */
  sh->memo.epoch = sh->alloc_epoch;
  sh->memo.start = p;
  sh->memo.end = limit;
  return 0;
}
