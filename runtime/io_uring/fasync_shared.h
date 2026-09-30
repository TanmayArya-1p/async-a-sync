#pragma once

#include <stddef.h>

#define FASYNC_REQ_FREE 0
#define FASYNC_REQ_PENDING 1
#define FASYNC_REQ_DONE 2
#define FASYNC_REQ_FAILED 3

/* sized so a whole workload fits in flight */
#define FASYNC_MAX_INFLIGHT 1024

#define FASYNC_ALLOC_WORDS (FASYNC_MAX_INFLIGHT / 64)

/* one in-flight request, shared with the native completion drain */
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
  void* task;           /* framework task owning buf, for the fasync_* API */
};

struct fasync_shared {
  volatile unsigned long* inflight; /* requests the kernel still holds */
  struct fasync_req_shared* reqs;
  unsigned long n_reqs;
  int ring_fd; /* for the blocking enter */

  /* one bit per slot so free slots skipped 64 at a time */
  unsigned long alloc_bits[FASYNC_ALLOC_WORDS];

  struct fasync_cqe* cqes;
  unsigned int* cq_head;
  unsigned int* cq_tail;
  unsigned int* cq_mask;
  unsigned int* local_cq_head;

  /* counters incremented from both halves */
  unsigned long* userspace_cq_polls;
  unsigned long* parks;
  unsigned long* kernel_wait_entries;
  unsigned long* completions_reaped;
};
