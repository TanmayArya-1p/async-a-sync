/* fasync.c -- the memory-safe core half. */
#include <stdfil.h>
#include <pizlonated_syscalls.h>

#include <string.h>

#include "fasync.h"
#include "fasync_io_uring.h"
#include "fasync_syscalls.h"
#include "fasync_shared.h"
#include "fasync_internal.h"

/* Ring depth matches the request table depth. */
#define FASYNC_RING_ENTRIES 1024

/* Must match FASYNC_NATIVE_SPIN_LIMIT in fasync_native.c. */
#define FASYNC_SPIN_LIMIT 20000

static struct fasync_req_shared req_slots[FASYNC_MAX_INFLIGHT];

static void fasync_req_table_init(void);
static unsigned int req_next_gen = 0;

static volatile unsigned long g_inflight = 0;

struct fasync_ring {
  int fd;
  int ready;

  struct fasync_sqe* sqes;
  unsigned int* sq_head;
  unsigned int* sq_tail;
  unsigned int* sq_mask;
  unsigned int* sq_array;

  struct fasync_cqe* cqes;
  unsigned int* cq_head;
  unsigned int* cq_tail;
  unsigned int* cq_mask;

  unsigned int sqe_tail;      /* next slot to write */
  unsigned int sqe_head;      /* next slot to publish */
  unsigned int queued;        /* written but not yet published */
  unsigned int local_cq_head; /* our view of the CQ head */
};

static struct fasync_ring g_ring;

static struct fasync_stats g_stats;

static struct fasync_shared g_shared;

static const char* g_last_error = "";

void fasync_reset_stats(void) { memset(&g_stats, 0, sizeof(g_stats)); }

void fasync_get_stats(struct fasync_stats* out) {
  if (!out)
    return;
  *out = g_stats;
}

const char* fasync_last_error(void) { return g_last_error; }

/* NO_MMAP because Fil-C cannot map io_uring rings. */
static int fasync_ring_init(void) {
  struct fasync_params p;
  memset(&p, 0, sizeof(p));

  size_t rings_bytes = FASYNC_RINGS_BYTES;
  size_t sqes_bytes = (size_t)FASYNC_RING_ENTRIES * sizeof(struct fasync_sqe);
  if (sqes_bytes < 4096)
    sqes_bytes = 4096;

  void* rings = zgc_aligned_alloc(4096, rings_bytes);
  void* sqes = zgc_aligned_alloc(4096, sqes_bytes);
  if (!rings || !sqes) {
    g_last_error = "out of memory allocating ring memory";
    return -1;
  }

  p.flags = FASYNC_SETUP_NO_MMAP;
  p.cq_off.user_addr = (unsigned long)(size_t)rings;
  p.sq_off.user_addr = (unsigned long)(size_t)sqes;

  long fd = zsys_io_uring_setup(FASYNC_RING_ENTRIES, (void*)&p);
  if (fd < 0) {
    g_last_error = "io_uring_setup failed";
    return -1;
  }
  g_ring.fd = (int)fd;

  g_ring.sqes = (struct fasync_sqe*)sqes;
  g_ring.sq_head = (unsigned int*)((char*)rings + p.sq_off.head);
  g_ring.sq_tail = (unsigned int*)((char*)rings + p.sq_off.tail);
  g_ring.sq_mask = (unsigned int*)((char*)rings + p.sq_off.ring_mask);
  g_ring.sq_array = (unsigned int*)((char*)rings + p.sq_off.array);

  g_ring.cqes = (struct fasync_cqe*)((char*)rings + p.cq_off.cqes);
  g_ring.cq_head = (unsigned int*)((char*)rings + p.cq_off.head);
  g_ring.cq_tail = (unsigned int*)((char*)rings + p.cq_off.tail);
  g_ring.cq_mask = (unsigned int*)((char*)rings + p.cq_off.ring_mask);

  g_ring.sqe_tail = 0;
  g_ring.sqe_head = 0;
  g_ring.queued = 0;
  g_ring.local_cq_head = 0;
  g_ring.ready = 1;

  fasync_req_table_init();

  /* Retained not copied so this memory must not move. */
  g_shared.inflight = &g_inflight;
  g_shared.reqs = req_slots;
  g_shared.n_reqs = FASYNC_MAX_INFLIGHT;
  g_shared.ring_fd = g_ring.fd;
  g_shared.cqes = g_ring.cqes;
  g_shared.cq_head = g_ring.cq_head;
  g_shared.cq_tail = g_ring.cq_tail;
  g_shared.cq_mask = g_ring.cq_mask;
  g_shared.local_cq_head = &g_ring.local_cq_head;
  g_shared.sq_tail = g_ring.sq_tail;
  g_shared.sq_mask = g_ring.sq_mask;
  g_shared.sq_array = g_ring.sq_array;
  g_shared.sqe_head = &g_ring.sqe_head;
  g_shared.sqe_tail = &g_ring.sqe_tail;
  g_shared.queued = &g_ring.queued;
  g_shared.userspace_cq_polls = &g_stats.userspace_cq_polls;
  g_shared.resolve_calls = &g_stats.resolve_calls;
  g_shared.fast_path_hits = &g_stats.fast_path_hits;
  g_shared.spin_rounds = &g_stats.spin_rounds;
  g_shared.parks = &g_stats.parks;
  g_shared.kernel_wait_entries = &g_stats.kernel_wait_entries;
  g_shared.kernel_submit_entries = &g_stats.kernel_submit_entries;
  g_shared.completions_reaped = &g_stats.completions_reaped;
  g_shared.memo_hits = &g_stats.memo_hits;

  fasync_publish_state(&g_shared);
  return 0;
}

static int fasync_ensure_ring(void) {
  if (g_ring.ready)
    return 0;
  return fasync_ring_init();
}

/* Free list with one-based slots so zero means empty. */
static unsigned int g_req_free_head;
static unsigned int g_req_free_next[FASYNC_MAX_INFLIGHT];

static void fasync_slot_mark(unsigned int index, int allocated) {
  unsigned long bit = 1UL << (index % 64);
  if (allocated)
    g_shared.alloc_bits[index / 64] |= bit;
  else
    g_shared.alloc_bits[index / 64] &= ~bit;
}

static void fasync_req_table_init(void) {
  memset(g_shared.alloc_bits, 0, sizeof(g_shared.alloc_bits));
  g_shared.memo.epoch = 0;
  g_shared.memo.start = 0;
  g_shared.memo.end = 0;
  for (unsigned int i = 0; i < FASYNC_MAX_INFLIGHT; i++) {
    req_slots[i].state = FASYNC_REQ_FREE;
    /* Slot i's next is i+2 so slot 0 is never handed out. */
    g_req_free_next[i] = (i + 2 <= FASYNC_MAX_INFLIGHT) ? i + 2 : 0;
  }
  g_req_free_head = 1; /* slot 0 */
}

static struct fasync_req_shared* fasync_req_alloc(void) {
  unsigned int head = g_req_free_head;
  if (!head)
    return 0;

  unsigned int index = head - 1;
  g_req_free_head = g_req_free_next[index];

  struct fasync_req_shared* r = &req_slots[index];
  r->gen = ++req_next_gen;
  /* The id packs slot and generation so reuse cannot alias. */
  r->id = ((fasync_id)r->gen << 32) | (fasync_id)index;
  r->state = FASYNC_REQ_PENDING;
  r->result = 0;
  r->linked = 0;
  fasync_slot_mark(index, 1);
  return r;
}

void fasync_req_release(struct fasync_req_shared* r) {
  unsigned int index = (unsigned int)(r->id & 0xFFFFFFFFUL);
  r->state = FASYNC_REQ_FREE;
  fasync_slot_mark(index, 0);
  g_req_free_next[index] = g_req_free_head;
  g_req_free_head = index + 1;
}

struct fasync_req_shared* fasync_req_lookup(fasync_id id) {
  unsigned int index = (unsigned int)(id & 0xFFFFFFFFUL);
  unsigned long gen = (unsigned long)(id >> 32);
  if (index >= FASYNC_MAX_INFLIGHT)
    return 0;
  struct fasync_req_shared* r = &req_slots[index];
  if (r->gen != gen || r->state == FASYNC_REQ_FREE)
    return 0;
  return r;
}

static struct fasync_req_shared* fasync_find_covering(const void* ptr, size_t size) {
  if (!g_ring.ready)
    return 0;
  return fasync_shared_find(&g_shared, ptr, size);
}

static struct fasync_sqe* fasync_get_sqe(void) {
  if (g_ring.queued >= FASYNC_RING_ENTRIES) {
    /* Ring full so publish what we have. */
    if (fasync_submit() < 0)
      return 0;
  }
  unsigned int index = g_ring.sqe_tail & *g_ring.sq_mask;
  return &g_ring.sqes[index];
}

/* addr and len are raw operands while result_buf names the waitable range. */
fasync_id fasync_push_sqe(unsigned char op, int fd, unsigned long addr,
                          unsigned int len, unsigned long offset,
                          void* result_buf, size_t result_len,
                          unsigned char sqe_flags) {
  if (fasync_ensure_ring() < 0)
    return 0;

  struct fasync_req_shared* r = fasync_req_alloc();
  if (!r) {
    g_last_error = "request table full";
    return 0;
  }

  struct fasync_sqe* sqe = fasync_get_sqe();
  if (!sqe) {
    fasync_req_release(r);
    return 0;
  }

  memset(sqe, 0, sizeof(*sqe));
  sqe->opcode = op;
  sqe->flags = sqe_flags;
  sqe->fd = fd;
  sqe->addr = addr;
  sqe->len = len;
  sqe->off = offset;
  sqe->user_data = r->id;

  g_ring.sqe_tail++;
  g_ring.queued++;
  g_stats.sqes_queued++;

  r->buf = result_buf;
  r->len = result_len;
  r->offset = offset;
  r->fd = fd;
  r->op = op;
  r->linked = (sqe_flags & FASYNC_SQE_IO_LINK) ? 1 : 0;

  /* Publish state before the count becomes visible. */
  g_shared.alloc_epoch++;
  __atomic_add_fetch(&g_inflight, 1, __ATOMIC_RELEASE);
  return r->id;
}

fasync_id fasync_push_buf(unsigned char op, int fd, void* buf, size_t len,
                          unsigned long offset, unsigned char sqe_flags) {
  return fasync_push_sqe(op, fd, (unsigned long)(size_t)buf, (unsigned int)len,
                         offset, buf, len, sqe_flags);
}

int fasync_submit(void) {
  if (!g_ring.ready || !g_ring.queued)
    return 0;

  unsigned int mask = *g_ring.sq_mask;
  for (unsigned int i = g_ring.sqe_head; i != g_ring.sqe_tail; i++)
    g_ring.sq_array[i & mask] = i & mask;

  /* The store that hands the SQEs to the kernel. */
  __atomic_store_n(g_ring.sq_tail, g_ring.sqe_tail, __ATOMIC_RELEASE);
  g_ring.sqe_head = g_ring.sqe_tail;

  unsigned int n = g_ring.queued;
  g_ring.queued = 0;

  /* min_complete zero submits and returns. */
  g_stats.kernel_submit_entries++;
  long ret = zsys_io_uring_enter(g_ring.fd, n, 0, 0);
  if (ret < 0) {
    g_last_error = "io_uring_enter(submit) failed";
    return -1;
  }
  return (int)n;
}

/* Explicit resolution with the same policy as the native hook. */
void* fasync_resolve_pending(void* ptr, size_t size) {
  if (!ptr)
    return ptr;

  if (__atomic_load_n(&g_inflight, __ATOMIC_ACQUIRE) == 0) {
    g_stats.fast_path_hits++;
    return ptr;
  }

  g_stats.resolve_calls++;

  struct fasync_req_shared* r = fasync_find_covering(ptr, size);
  if (!r)
    return ptr;

  /* The lazy batch publish submits everything in one enter. */
  fasync_submit();

  for (unsigned int spin = 0; spin < FASYNC_SPIN_LIMIT; spin++) {
    if (r->state != FASYNC_REQ_PENDING)
      return ptr;
    g_stats.spin_rounds++;
    fasync_poll();
  }

  while (r->state == FASYNC_REQ_PENDING) {
    g_stats.parks++;
    fasync_block();
    fasync_poll();
  }
  return ptr;
}

int fasync_wait_all(void) {
  fasync_submit();

  /* Bounded so a stuck request cannot hang the process. */
  for (unsigned long spin = 0; spin < 100000000UL; spin++) {
    if (__atomic_load_n(&g_inflight, __ATOMIC_ACQUIRE) == 0)
      return 0;

    fasync_poll();
    if (__atomic_load_n(&g_inflight, __ATOMIC_ACQUIRE) == 0)
      return 0;

    fasync_block();
  }
  return -1;
}

int fasync_provenance(const void* ptr, size_t size, struct fasync_prov* out) {
  struct fasync_req_shared* r = fasync_find_covering(ptr, size);
  if (!r || !out)
    return 0;
  out->req = r->id;
  out->offset = (unsigned long)((const char*)ptr - (const char*)r->buf);
  out->len = (unsigned long)r->len;
  return 1;
}

void* fasync_derive(void* base, unsigned long offset, size_t len,
                    struct fasync_prov* out) {
  char* derived = (char*)base + offset;
  if (out) {
    if (!fasync_provenance(derived, len, out)) {
      out->req = 0;
      out->offset = offset;
      out->len = (unsigned long)len;
    }
  }
  return derived;
}