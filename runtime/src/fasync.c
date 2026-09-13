/*
 * fasync.c -- implicit futures for syscalls. The memory-safe half.
 *
 * This file is compiled by filcc and is therefore subject to the full Fil-C
 * capability model: every pointer here is checked, and there is no way to
 * escape that. The only privileged operations it performs are delegated to
 * fasync_native.c, which is the minimal unsafe shim.
 *
 * WHAT LIVES HERE
 * ---------------
 *   - the io_uring submission/completion rings (setup, publish, poll)
 *   - the pending-request table that maps a request id to a buffer range
 *   - filc_resolve_pending(), the hook the FilPizlonator patch calls
 *   - resolution policy: spin against the completion queue first, park only if
 *     the completion genuinely has not landed
 *   - provenance: which request a (possibly derived) pointer came from
 *
 * WHAT DOES NOT LIVE HERE (yet)
 * -----------------------------
 *   - the dependency DAG and disjointness proof (fasync_dep.c)
 *   - direct-descriptor promise pipelining (fasync_pipe.c)
 *
 * RESOLUTION SEMANTICS
 * --------------------
 * idea.md section 2.6 offers two ways to "resolve" a pending capability: swap
 * the capability to point at a real buffer, or flip the pending bit in place.
 * This implementation flips the bit in place: the buffer address never changes,
 * because the kernel was already told where to write. Resolution therefore
 * means "the contents are now durable", and the pointer value is unchanged.
 * That keeps the fast path to a single load, which is what makes the check
 * cheap enough to put on every access.
 *
 * The consequence worth being explicit about: an access that merely *reads* a
 * pending buffer's address (without dereferencing it) is not delayed, and
 * neither is a write to it from this program. Only genuine accesses through the
 * checked path wait. That matches the "lazy resolution on first genuine access"
 * rule, and it is why aliasing writes are called out as a real hazard in
 * docs/ARCHITECTURE.md.
 */

#include <stdfil.h>
#include <pizlonated_syscalls.h>

#include <errno.h>
#include <fcntl.h>
#include <string.h>

#include "fasync.h"

/* The io_uring kernel ABI (SQE/CQE ring layouts, opcodes, syscall numbers).
 * Shared with fasync_native.c, which must agree with this file byte for byte. */
#include "fasync_io_uring.h"

/* The io_uring syscalls, as added to libpizlo's trusted runtime. */
#include "fasync_syscalls.h"
#include "fasync_shared.h"

/* ------------------------------------------------------------------ */
/* Configuration                                                        */
/* ------------------------------------------------------------------ */

/* How many requests may be in flight at once. Also the size of the request
 * table, which is scanned linearly when resolving -- acceptable at this size
 * and deliberately simple. */
#define FASYNC_MAX_INFLIGHT 256

/* Ring depth: how many SQEs may be queued before the kernel must be told. */
#define FASYNC_RING_ENTRIES 128

/* Userspace spin budget before we agree to sleep in the kernel. Polling the
 * completion queue is a plain memory read of a shared ring, so spinning is
 * cheap; parking is the expensive operation we are trying to avoid. */
#define FASYNC_SPIN_LIMIT 20000

/* ------------------------------------------------------------------ */
/* Request table                                                       */
/*                                                                     */
/* The layout lives in fasync_shared.h rather than here, because the      */
/* native resolver reads the same table through raw pointers and the two  */
/* halves must agree field for field. The state constants come from there  */
/* too.                                                                    */
/* ------------------------------------------------------------------ */

static struct fasync_req_shared req_slots[FASYNC_MAX_INFLIGHT];
static unsigned int req_next_gen = 1;

/*
 * The fast-path gate.
 *
 * filc_resolve_pending() runs on every access, so its first act is a single
 * acquire load of this counter. When nothing is in flight the function returns
 * immediately, having touched no lock, no table, and no syscall. This is the
 * userspace equivalent of a Brooks forwarding-pointer check: predictable,
 * cache-resident, and only expensive when there is genuinely something to
 * resolve.
 */
static volatile unsigned long g_inflight = 0;

/* ------------------------------------------------------------------ */
/* Pending descriptors                                                 */
/* ------------------------------------------------------------------ */

/*
 * Provenance for file descriptors.
 *
 * A buffer's provenance is a range: the request that fills it. A descriptor's
 * provenance is a *slot*: the request that will produce it. Until the open
 * completes there is no fd to hand out, so fasync_open_pending returns a negative
 * handle instead, and every operation that takes an fd resolves it first. Passing
 * a pending descriptor to a read is therefore what creates the dependency, with
 * nothing declared and nothing to remember.
 *
 * Handles are negative because a real descriptor never is, so resolution costs
 * one comparison in the common case. They start at -2: -1 is reserved for
 * failure, and letting slot 0 alias it was a real bug.
 *
 * What this does NOT buy is overlap: the read cannot be submitted until the open
 * has actually produced an fd, so the open and the read do not run concurrently.
 * Doing better needs the kernel to chain them (IOSQE_IO_LINK with the read naming
 * a not-yet-populated direct descriptor), which needs the open to target an
 * explicit slot. On this machine the kernel ignores an explicit slot and
 * allocates its own, so the read has nothing to name in advance --
 * tests/stage6b_fd_chain_probe.c is the evidence. See docs/ARCHITECTURE.md.
 */
#define FASYNC_MAX_PENDING_FDS 64

struct fasync_pending_fd {
  int used;
  fasync_id id; /* the request that will produce the fd */
  long fd;      /* resolved value, or -1 while still pending */
};

static struct fasync_pending_fd g_pending_fds[FASYNC_MAX_PENDING_FDS];

static int fasync_pending_fd_new(fasync_id id) {
  for (int i = 0; i < FASYNC_MAX_PENDING_FDS; i++) {
    if (g_pending_fds[i].used)
      continue;
    g_pending_fds[i].used = 1;
    g_pending_fds[i].id = id;
    g_pending_fds[i].fd = -1;
    /* Encoded as -(index + 2) so that -1 remains an unambiguous failure and
     * slot 0 does not alias it. */
    return -(i + 2);
  }
  return -1;
}

/* ------------------------------------------------------------------ */
/* Ring state                                                          */
/* ------------------------------------------------------------------ */

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

  unsigned int sqe_tail;       /* next slot to write                      */
  unsigned int sqe_head;       /* next slot to publish                    */
  unsigned int queued;         /* SQEs written but not yet published      */
  unsigned int local_cq_head;  /* our view of the completion queue head   */
};

static struct fasync_ring g_ring;

static struct fasync_stats g_stats;

/* Everything the native resolver needs, published once at ring setup. Lives
 * here (not natively) because the ring and the request table are owned by this
 * half; the native side holds raw pointers into them. */
static struct fasync_shared g_shared;

/* Most recent failure, as a static string. The runtime deliberately does not
 * use stdio: reporting through return values keeps this library free of a
 * dependency on which archive member resolves first. */
static const char* g_last_error = "";

/* ------------------------------------------------------------------ */
/* Stats helpers                                                       */
/* ------------------------------------------------------------------ */

void fasync_reset_stats(void) { memset(&g_stats, 0, sizeof(g_stats)); }

void fasync_get_stats(struct fasync_stats* out) {
  if (!out)
    return;
  *out = g_stats;
}

const char* fasync_last_error(void) { return g_last_error; }

/* ------------------------------------------------------------------ */
/* Ring setup                                                          */
/* ------------------------------------------------------------------ */

/*
 * Ring memory is ours, not the kernel's.
 *
 * The obvious approach -- let the kernel create the rings and mmap them -- does
 * not work on Fil-C, and the reason is structural rather than a bug in either
 * side. Fil-C's mmap wrapper (filc_native_zsys_mmap) must guarantee that the
 * address a mapping lands at is the address it hands back a capability for. To
 * do that it pre-allocates the address out of the GC heap and passes MAP_FIXED.
 * But the kernel rejects MAP_FIXED for io_uring ring mappings:
 *
 *     mmap(NULL,  640, PROT_READ|PROT_WRITE, MAP_SHARED|MAP_POPULATE, fd, 0) -> OK
 *     mmap(addr,  640, PROT_READ|PROT_WRITE, MAP_SHARED|MAP_POPULATE|MAP_FIXED, fd, 0) -> EINVAL
 *
 * So a ring mapping can never carry a Fil-C capability, and the memory-safe half
 * could never read the completion queue out of it.
 *
 * IORING_SETUP_NO_MMAP (kernel 6.5+) inverts the arrangement: the *caller*
 * supplies the ring memory and the kernel uses it in place. The memory is
 * therefore ordinary Fil-C GC memory, which means:
 *
 *   - the completion queue is directly readable from memory-safe code, so the
 *     poll stays a plain load with no syscall and no copy, and
 *   - the "no escape hatch" property is preserved: nothing here needs an
 *     uncapabilitied pointer.
 *
 * The one hazard is that GC memory must not move, since the kernel caches the
 * address. stage2c_gc_pin_probe.c checks that directly (allocates, forces
 * collection cycles, verifies address and contents are unchanged); large
 * page-aligned allocations from zgc_aligned_alloc are stable and are not
 * relocated by the scavenger.
 */
static int fasync_ring_init(void) {
  struct fasync_params p;
  memset(&p, 0, sizeof(p));

  /* The rings have to be one contiguous region shared by SQ and CQ; the exact
   * size is only reported by the kernel after setup, so this is a generous
   * upper bound (the kernel touches only what it needs). The SQEs are a
   * separate region. */
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

  /* Ring fields live at kernel-reported offsets inside the caller-provided
   * region; the kernel filled these in during setup. */
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

  memset(req_slots, 0, sizeof(req_slots));

  /*
   * Publish what the native resolver needs. It retains these addresses rather
   * than copying them, so they must stay valid and stable for the process's
   * lifetime: the ring is GC memory (stable, see stage2c_gc_pin_probe) and the
   * request table is a static array.
   */
  g_shared.inflight = &g_inflight;
  g_shared.reqs = req_slots;
  g_shared.n_reqs = FASYNC_MAX_INFLIGHT;
  g_shared.ring_fd = g_ring.fd;
  g_shared.cqes = g_ring.cqes;
  g_shared.cq_head = g_ring.cq_head;
  g_shared.cq_tail = g_ring.cq_tail;
  g_shared.cq_mask = g_ring.cq_mask;
  g_shared.local_cq_head = &g_ring.local_cq_head;
  g_shared.userspace_cq_polls = &g_stats.userspace_cq_polls;
  g_shared.resolve_calls = &g_stats.resolve_calls;
  g_shared.fast_path_hits = &g_stats.fast_path_hits;
  g_shared.spin_rounds = &g_stats.spin_rounds;
  g_shared.parks = &g_stats.parks;
  g_shared.kernel_wait_entries = &g_stats.kernel_wait_entries;
  g_shared.completions_reaped = &g_stats.completions_reaped;

  fasync_publish_state(&g_shared);
  return 0;
}

static int fasync_ensure_ring(void) {
  if (g_ring.ready)
    return 0;
  return fasync_ring_init();
}

/* ------------------------------------------------------------------ */
/* Request table                                                       */
/* ------------------------------------------------------------------ */

static struct fasync_req_shared* fasync_req_alloc(void) {
  for (unsigned int i = 0; i < FASYNC_MAX_INFLIGHT; i++) {
    struct fasync_req_shared* r = &req_slots[i];
    if (r->state != FASYNC_REQ_FREE)
      continue;
    r->gen = req_next_gen++;
    /* The id packs a slot index in the low 32 bits and a generation above it,
     * so that a handle held across a slot recycle is detected rather than
     * silently resolving to an unrelated request. */
    r->id = ((fasync_id)r->gen << 32) | (fasync_id)i;
    r->state = FASYNC_REQ_PENDING;
    r->result = 0;
    r->linked = 0;
    return r;
  }
  return 0;
}

static struct fasync_req_shared* fasync_req_lookup(fasync_id id) {
  unsigned int index = (unsigned int)(id & 0xFFFFFFFFUL);
  unsigned long gen = (unsigned long)(id >> 32);
  if (index >= FASYNC_MAX_INFLIGHT)
    return 0;
  struct fasync_req_shared* r = &req_slots[index];
  if (r->gen != gen || r->state == FASYNC_REQ_FREE)
    return 0;
  return r;
}

/*
 * Find the pending request (if any) whose result buffer covers [ptr, ptr+size).
 *
 * Only pending requests are considered: a completed request is no longer a
 * hazard, and excluding it is the "self-healing" step -- once resolved, the
 * range stops being reported as pending, so subsequent accesses fall straight
 * through to the fast path.
 */
static struct fasync_req_shared* fasync_find_covering(const void* ptr, size_t size) {
  const char* p = (const char*)ptr;
  for (unsigned int i = 0; i < FASYNC_MAX_INFLIGHT; i++) {
    struct fasync_req_shared* r = &req_slots[i];
    if (r->state != FASYNC_REQ_PENDING || !r->buf)
      continue;
    const char* start = (const char*)r->buf;
    const char* end = start + r->len;
    if (p < start)
      continue;
    if (p + size > end)
      continue;
    return r;
  }
  return 0;
}

/* ------------------------------------------------------------------ */
/* Submission                                                          */
/* ------------------------------------------------------------------ */

static struct fasync_sqe* fasync_get_sqe(void) {
  if (g_ring.queued >= FASYNC_RING_ENTRIES) {
    /* The SQ ring is full. Publish what we have so the kernel can start work,
     * which is also the only correct way to make room. */
    if (fasync_submit() < 0)
      return 0;
  }
  unsigned int index = g_ring.sqe_tail & *g_ring.sq_mask;
  return &g_ring.sqes[index];
}

/*
 * Hand a queued SQE to the kernel and return the freshly allocated request.
 *
 * Note what does NOT happen here: no waiting, and no syscall. The SQE is
 * written into shared memory and the ring tail is bumped in fasync_submit().
 * The caller gets a handle back immediately -- this is mechanism 1 of
 * idea.md section 1, "zero-context-switch submission".
 *
 * `addr`/`len` are the raw SQE operands and do not always describe a buffer:
 * for openat, `addr` is the path and `len` is the mode. `result_buf` separately
 * names the range whose *contents* the caller will later have to wait for, or 0
 * for operations that produce a scalar (an fd, an error code) rather than
 * filling a buffer. Keeping those two concepts apart matters because only
 * result ranges participate in resolution and provenance tracking.
 */
static fasync_id fasync_push_sqe(unsigned char op, int fd, unsigned long addr,
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
    r->state = FASYNC_REQ_FREE;
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

  /* Publish the pending state before the count becomes visible, so a resolver
   * that observes a non-zero in-flight count is guaranteed to see this
   * request. */
  __atomic_add_fetch(&g_inflight, 1, __ATOMIC_RELEASE);
  return r->id;
}

/* Convenience wrapper for the common case: an operation whose SQE operands are
 * exactly the buffer it fills. */
static fasync_id fasync_push_buf(unsigned char op, int fd, void* buf, size_t len,
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

  /* Publish the tail. This is the store that makes the SQEs visible to the
   * kernel; with SQPOLL the kernel picks them up with no syscall at all. */
  __atomic_store_n(g_ring.sq_tail, g_ring.sqe_tail, __ATOMIC_RELEASE);
  g_ring.sqe_head = g_ring.sqe_tail;

  unsigned int n = g_ring.queued;
  g_ring.queued = 0;

  /* min_complete = 0: submit and return. Still a syscall, but it neither
   * blocks nor waits, and it batches every SQE queued so far. */
  g_stats.kernel_submit_entries++;
  long ret = zsys_io_uring_enter(g_ring.fd, n, 0, 0);
  if (ret < 0) {
    g_last_error = "io_uring_enter(submit) failed";
    return -1;
  }
  return (int)n;
}

