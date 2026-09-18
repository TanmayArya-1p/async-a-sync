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

/* Defined below; needed by the ring setup path above it. */
static void fasync_req_table_init(void);
static unsigned int req_next_gen = 0;

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

  fasync_req_table_init();

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

/*
 * Allocation is a free list, not a scan.
 *
 * The table used to be searched linearly for a FREE slot on every submission,
 * which is O(in-flight) per request -- fine at a handful of requests and
 * ruinous for the many-small-operations case the whole design is aimed at. The
 * throughput test is what made that visible: it was saving 32x the kernel
 * entries and still coming out slower than a plain read loop.
 *
 * Slots indices are stored one-based so that 0 can mean "empty".
 */
static unsigned int g_req_free_head;
static unsigned int g_req_free_next[FASYNC_MAX_INFLIGHT];

static void fasync_req_table_init(void) {
  for (unsigned int i = 0; i < FASYNC_MAX_INFLIGHT; i++) {
    req_slots[i].state = FASYNC_REQ_FREE;
    /* Indices are stored one-based, so the link to slot i+1 is i+2. Getting
     * this wrong hands out slot 0 forever, which is a hang rather than a
     * wrong answer: several requests share one slot and only the last one's
     * completion can ever be matched. */
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
  /* The id packs a slot index in the low 32 bits and a generation above it, so
   * that a handle held across a slot recycle is detected rather than silently
   * resolving to an unrelated request. */
  r->id = ((fasync_id)r->gen << 32) | (fasync_id)index;
  r->state = FASYNC_REQ_PENDING;
  r->result = 0;
  r->linked = 0;
  return r;
}

static void fasync_req_release(struct fasync_req_shared* r) {
  unsigned int index = (unsigned int)(r->id & 0xFFFFFFFFUL);
  r->state = FASYNC_REQ_FREE;
  g_req_free_next[index] = g_req_free_head;
  g_req_free_head = index + 1;
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

/* ------------------------------------------------------------------ */
/* Completion                                                          */
/* ------------------------------------------------------------------ */

/*
 * Resolution for the explicit-access path.
 *
 * The compiler-inserted hook (filc_resolve_pending, native) is the hot path and
 * keeps the tight spin against the completion ring. This is the other path --
 * code that calls fasync_resolve_pending() directly -- and it uses the same
 * policy expressed with the two scalar bridged calls: drain the completion ring
 * from userspace (no syscall), and only sleep once the spin budget is gone.
 *
 * This lives on the safe side on purpose. An earlier revision bridged the whole
 * policy out to a native function taking a filc_ptr, and that crashed
 * intermittently in the generated marshalling; see the note in
 * generate_pizlonated_forwarders.rb. The bridged surface is now scalars and void
 * returns only.
 */
/* (the spin budget is defined once, above) */

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

/* ------------------------------------------------------------------ */
/* Public async syscalls                                               */
/* ------------------------------------------------------------------ */

/*
 * Each of these validates its buffer against the Fil-C capability model before
 * the kernel is allowed near it. This mirrors exactly what the synchronous
 * zsys_read / zsys_write paths do, which is what idea.md section 2.6 means by
 * "syscalls are already a checked boundary": going async does not get to skip
 * the check, it just moves the waiting somewhere else.
 */

fasync_id fasync_pread(int fd, void* buf, size_t len, unsigned long offset) {
  /* If this descriptor is itself an async open that has not completed, this is
   * where the dependency is honoured: the read cannot be submitted until there
   * is a descriptor to read from. */
  long real = fasync_fd_resolve(fd);
  if (real < 0)
    return 0;
  fd = (int)real;

  zcheck(buf, len); /* in bounds and writable */
  return fasync_push_buf(FASYNC_OP_READ, fd, buf, len, offset, 0);
}

fasync_id fasync_pwrite(int fd, void* buf, size_t len, unsigned long offset) {
  long real = fasync_fd_resolve(fd);
  if (real < 0)
    return 0;
  fd = (int)real;

  zcheck_readonly(buf, len); /* in bounds; the kernel only reads it */

  /*
   * Resolve the source eagerly.
   *
   * This is idea.md section 3.1's second case: the dependency is not "code
   * touches the buffer", it is "the *kernel* needs these bytes as the content of
   * this SQE". We cannot defer that one, because there is no way to tell the
   * kernel "write whatever ends up in this buffer" -- it reads the bytes when it
   * executes the request, and if a prior async read into the same buffer has not
   * landed yet it would write garbage.
   *
   * So a write's source is a genuine synchronisation point and is resolved here,
   * rather than lazily on a later access. That costs us the overlap between this
   * write and the read that feeds it, which is precisely the case the design
   * cannot get for free. The kernel-native fix is to chain the two requests with
   * IOSQE_IO_LINK so the write only executes after the read completes; see the
   * pipelining section of docs/ARCHITECTURE.md for where that work stands.
   */
  fasync_resolve_pending(buf, len);

  return fasync_push_sqe(FASYNC_OP_WRITE, fd, (unsigned long)(size_t)buf,
                         (unsigned int)len, offset, 0, 0, 0);
}

fasync_id fasync_fsync(int fd) {
  long real = fasync_fd_resolve(fd);
  if (real < 0)
    return 0;
  return fasync_push_sqe(FASYNC_OP_FSYNC, (int)real, 0, 0, 0, 0, 0, 0);
}

fasync_id fasync_close(int fd) {
  long real = fasync_fd_resolve(fd);
  if (real < 0)
    return 0;
  return fasync_push_sqe(FASYNC_OP_CLOSE, (int)real, 0, 0, 0, 0, 0, 0);
}

/*
 * Open a file, returning a *pending descriptor* rather than a handle: a negative
 * value that can be handed straight to fasync_pread/pwrite/fsync/close, or
 * resolved explicitly with fasync_fd_resolve. Nothing about the call site marks
 * it as asynchronous, which is the point.
 */
int fasync_open_pending(int dirfd, const char* path, int flags, int mode) {
  fasync_id id = fasync_openat(dirfd, path, flags, mode);
  if (!id)
    return -1;
  return fasync_pending_fd_new(id);
}

fasync_id fasync_openat(int dirfd, const char* path, int flags, int mode) {
  if (!path) {
    errno = EFAULT;
    return 0;
  }
  /* The path is a NUL-terminated string with no length argument, so the
   * capability check is for the single terminator byte; the runtime's own
   * string handling rejects a path that runs off the end of its allocation. */
  zcheck_readonly((void*)path, 1);

  /* For IORING_OP_OPENAT the SQE's `len` field carries the file mode, and its
   * `addr` carries the path. Neither is a result buffer: the operation yields
   * an fd, so result_buf is 0. */
  return fasync_push_sqe(FASYNC_OP_OPENAT, dirfd, (unsigned long)(size_t)path,
                         (unsigned int)mode, (unsigned long)flags, 0, 0, 0);
}

/*
 * NOTE: fasync_openat_direct() -- openat that allocates into a direct
 * descriptor slot, so a dependent read can be submitted against a descriptor
 * that does not exist yet -- is deliberately absent. The SQE plumbing is
 * trivial (set sqe->file_index), but it is only meaningful once the ring has a
 * registered file table, and the exact IORING_FILE_INDEX_ALLOC contract wants
 * to be verified against the running kernel before it is relied on. It lands
 * with the pipelining work, where it is actually exercised.
 */

/* ------------------------------------------------------------------ */
/* Results                                                             */
/* ------------------------------------------------------------------ */

int fasync_ready(fasync_id id) {
  struct fasync_req_shared* r = fasync_req_lookup(id);
  if (!r)
    return 0;
  if (r->state == FASYNC_REQ_PENDING)
    fasync_poll();
  return r->state != FASYNC_REQ_PENDING;
}

int fasync_wait_all(void) {
  fasync_submit(); /* publish anything still queued */

  /* Bounded so that a request which never completes cannot hang the process; the
   * failure mode this guards against is the one idea.md section 2.6 flags as
   * owned by the runtime rather than by the kernel. */
  for (unsigned long spin = 0; spin < 100000000UL; spin++) {
    if (__atomic_load_n(&g_inflight, __ATOMIC_ACQUIRE) == 0)
      return 0;

    fasync_poll();
    if (__atomic_load_n(&g_inflight, __ATOMIC_ACQUIRE) == 0)
      return 0;

    /* Nothing left to reap, so sleep until the kernel has something rather than
     * spinning through enters. */
    fasync_block();
  }
  return -1;
}

long fasync_result(fasync_id id) {
  struct fasync_req_shared* r = fasync_req_lookup(id);
  if (!r)
    return -EINVAL;

  /*
   * Publish anything still queued before waiting.
   *
   * Without this, waiting on a request whose SQE has not been handed to the
   * kernel yet waits forever -- and the natural way to hit that is resolving a
   * pending descriptor without an explicit fasync_submit(), which is exactly the
   * ergonomic path this is supposed to support. Submitting here is a no-op when
   * nothing is queued.
   */
  if (r->state == FASYNC_REQ_PENDING)
    fasync_submit();

  while (r->state == FASYNC_REQ_PENDING) {
    fasync_poll();
    if (r->state == FASYNC_REQ_PENDING)
      fasync_block();
  }

  if (r->state == FASYNC_REQ_PENDING) {
    /* Still pending even after parking: the kernel has not reported this
     * request. This is the "pending capability that never resolves" failure
     * mode flagged in idea.md section 2.6 -- we own it, and surfacing it as an
     * error is strictly better than hanging or reading garbage. */
    return -ETIMEDOUT;
  }

  long result = r->result;
  fasync_req_release(r);
  return result;
}

/*
 * Resolve a descriptor: a real fd passes straight through, a pending handle is
 * waited on and replaced by the fd it produced. This is the whole of fd
 * provenance on the consumer side, and it is why an operation can be written
 * against a descriptor that does not exist yet.
 */
long fasync_fd_resolve(int fd) {
  if (fd >= 0)
    return fd; /* already a real descriptor */

  if (fd == -1)
    return -EBADF; /* the reserved failure value, not a handle */
  int index = -fd - 2;
  if (index < 0 || index >= FASYNC_MAX_PENDING_FDS)
    return -EBADF;

  struct fasync_pending_fd* p = &g_pending_fds[index];
  if (!p->used)
    return -EBADF;

  if (p->fd < 0) {
    long result = fasync_result(p->id);
    if (result < 0)
      return result;
    p->fd = result;
  }
  return p->fd;
}

/* ------------------------------------------------------------------ */
/* Provenance -- mechanism 2 of idea.md section 1                      */
/* ------------------------------------------------------------------ */

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
