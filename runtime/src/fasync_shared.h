#pragma once

#include <stddef.h>

#define FASYNC_REQ_FREE 0
#define FASYNC_REQ_PENDING 1
#define FASYNC_REQ_DONE 2
#define FASYNC_REQ_FAILED 3

/* sized so a whole workload fits in flight */
#define FASYNC_MAX_INFLIGHT 1024

#define FASYNC_ALLOC_WORDS (FASYNC_MAX_INFLIGHT / 64)

/* gaps the memo keeps, so a loop reading a buffer and memory elsewhere (a
 * global, a table) keeps one for each */
#define FASYNC_MEMO_WAYS 4

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

  /* bumped on every allocation to invalidate the memo; starts at 1 so a
   * zeroed memo entry is never current */
  unsigned long alloc_epoch;

  /* ranges known to contain no pending result buffer, each the whole gap
   * between two pending buffers */
  struct fasync_memo {
    unsigned long epoch;
    const char* start; /* 0 for the bottom of memory */
    const char* end;   /* 0 for the top */
  } memo[FASYNC_MEMO_WAYS];
  unsigned int memo_next; /* the entry the next miss replaces */

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
  for (unsigned int i = 0; i < FASYNC_MEMO_WAYS; i++) {
    const struct fasync_memo* m = &sh->memo[i];
    if (m->epoch != sh->alloc_epoch)
      continue;
    if (m->start && p < m->start)
      continue;
    if (m->end && p + size > m->end)
      continue;
    return 1;
  }
  return 0;
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

  const char* below = 0; /* end of the nearest pending buffer below p */
  const char* limit = 0; /* start of the nearest pending buffer above p */
  int partial = 0;       /* a pending buffer holds part of the range */
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
      if (start < p + size && p < end)
        partial = 1;
      else if (start >= p + size && (!limit || start < limit))
        limit = start;
      else if (end <= p && end > below)
        below = end;
    }
    if (found)
      break;
  }

  if (found)
    return found;

  /* Nothing pending in [below, limit), so remember that gap, unless part of
   * the range is pending: then no gap around p is free of pending buffers. */
  if (!partial) {
    struct fasync_memo* m = &sh->memo[sh->memo_next];
    sh->memo_next = (sh->memo_next + 1) % FASYNC_MEMO_WAYS;
    m->epoch = sh->alloc_epoch;
    m->start = below;
    m->end = limit;
  }
  return 0;
}
