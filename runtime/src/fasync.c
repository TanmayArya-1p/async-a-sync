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

