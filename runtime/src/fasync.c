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

