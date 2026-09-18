/*
 * fasync.h -- implicit futures for syscalls, on top of Fil-C + io_uring.
 *
 * This is the public surface of the runtime extension. It is what demo
 * programs link against, and -- importantly -- what the FilPizlonator patch
 * calls. The compiler patch does not need to understand any of this: it only
 * needs filc_resolve_pending().
 *
 * THE THREE MECHANISMS (idea.md section 1)
 * ----------------------------------------
 * 1. Zero-context-switch submission. Every syscall becomes an io_uring SQE.
 *    fasync_* enqueues and returns immediately; nothing blocks at the call
 *    site. Publication happens on fasync_submit(), which uses a non-blocking
 *    io_uring_enter.
 *
 * 2. Provenance-tracked results. The handle returned by an async syscall
 *    identifies the request that produced it. Any pointer derived from the
 *    result by arithmetic or indexing inherits that tag, so the whole family
 *    of derived pointers stays traceable. See fasync_provenance().
 *
 * 3. Lazy resolution on first genuine access. filc_resolve_pending() is the
 *    point where the illusion is cashed in: if the pointer's request has not
 *    completed, it spins against the completion queue and then parks. If it
 *    has completed, the call is a single load and a predictable branch.
 *
 * WHY THERE IS A VISIBLE CALL SITE TODAY
 * --------------------------------------
 * filc_resolve_pending() is called *by the compiler*, not by the programmer.
 * The FilPizlonator patch inserts it adjacent to the range check that
 * FilPizlonator already emits on every access. Until that patched compiler is
 * built, demo programs mark the same points with the FASYNC_ACCESS() macro
 * below -- a macro that expands to exactly the call the compiler will emit, and
 * that is expected to disappear entirely once the patched compiler is in use.
 * See docs/ARCHITECTURE.md.
 */

#ifndef FASYNC_H
#define FASYNC_H

#include <stddef.h>

/* A handle for one in-flight async syscall. This is the "tag" that provenance
 * tracking is built on: it names the request, not the buffer. */
typedef unsigned long fasync_id;

/* The resolution logic, implemented in fasync.c. */
void* fasync_resolve_pending(void* ptr, size_t size);

/*
 * The hook the patched compiler emits calls to.
 *
 * Two symbols exist for this one operation, and the reason is Fil-C's naming
 * convention: a name starting with filc_ is treated as a runtime entry point and
 * is referenced *unprefixed*, whereas an ordinary external function called from
 * filcc-compiled code is referenced as pizlonated_<name>. The pass emits a direct
 * call to filc_resolve_pending, while code compiled from source (the macro below,
 * and fasync_pwrite) reaches pizlonated_fasync_resolve_pending. The unprefixed
 * form is a one-line bridge in fasync_native.c that forwards to the logic.
 */
void* filc_resolve_pending(void* ptr, size_t size);

/*
 * The points at which the patched compiler inserts a resolution check.
 *
 * With an unpatched compiler these are explicit calls. With a patched compiler
 * they are redundant (the compiler has already emitted its own
 * filc_resolve_pending()) and this macro is defined to nothing.
 */
#ifdef FASYNC_COMPILER_INSERTS_CHECKS
#define FASYNC_ACCESS(ptr, size) ((void)0)
#else
#define FASYNC_ACCESS(ptr, size) ((void)fasync_resolve_pending((void*)(ptr), (size)))
#endif

/* ------------------------------------------------------------------ */
/* Effect-set kinds, for the dependency layer (idea.md section 3.2).   */
/* ------------------------------------------------------------------ */
enum fasync_access_kind {
  FASYNC_IN = 0,   /* operation reads this range   */
  FASYNC_OUT = 1,  /* operation writes this range  */
  FASYNC_INOUT = 2 /* operation may do both        */
};

/* ------------------------------------------------------------------ */
/* Fixed-size ABI types (the ring layout must not drift).              */
/* ------------------------------------------------------------------ */
typedef unsigned int fasync_uint32;

/* ------------------------------------------------------------------ */
/* Async syscalls.                                                     */
/*                                                                     */
/* Each returns a handle immediately. Nothing here waits for the kernel. */
/* ------------------------------------------------------------------ */

/* Note: there is deliberately no fasync_read(). A plain read(2) has no offset,
 * and a promise-pipelined read depends on the file's cursor position at
 * execution time, not at submission time -- so it is excluded from the
 * supported subset for now. Use pread(2) (offset-carrying, and therefore
 * genuinely parallelizable), which is the case that shows the win. */

fasync_id fasync_pread(int fd, void* buf, size_t len, unsigned long offset);
fasync_id fasync_pwrite(int fd, void* buf, size_t len, unsigned long offset);
fasync_id fasync_fsync(int fd);
fasync_id fasync_close(int fd);
fasync_id fasync_openat(int dirfd, const char* path, int flags, int mode);

/* ------------------------------------------------------------------ */
/* Descriptor provenance                                               */
/*                                                                     */
/* A descriptor produced by an async open does not exist until that open */
/* completes. fasync_open_pending therefore returns a *pending*          */
/* descriptor -- a negative value, since a real descriptor never is --   */
/* which can be passed anywhere an fd is accepted. Every operation that  */
/* takes an fd resolves it first, so using a pending descriptor is what   */
/* creates the dependency. Nothing is declared and nothing is remembered. */
/* ------------------------------------------------------------------ */

/* Open and return a pending descriptor. */
int fasync_open_pending(int dirfd, const char* path, int flags, int mode);

/*
 * Resolve a descriptor: a real fd is returned unchanged, a pending one is
 * waited on. Returns the fd, or -errno. Blocks, so this is the one place fd
 * provenance gives up overlap.
 */
long fasync_fd_resolve(int fd);

/* fasync_openat_direct() -- an openat whose result is allocated into a direct
 * descriptor slot, so a dependent operation can be submitted against a
 * descriptor that does not exist yet (the kernel-native half of promise
 * pipelining) -- arrives with the pipelining work. See fasync.c. */

/* ------------------------------------------------------------------ */
/* Submission and completion.                                          */
/* ------------------------------------------------------------------ */

/* Publish all queued SQEs to the kernel. Non-blocking (min_complete = 0).
 * Returns the number submitted, or -1 on error. */
int fasync_submit(void);

/* Has this request's completion landed? Never blocks. */
int fasync_ready(fasync_id id);

/*
 * Wait until nothing is in flight, reaping completions in whatever order they
 * arrive.
 *
 * The throughput pattern: you care that the batch finished, not which request
 * finished first. Waiting on each handle in turn costs one kernel entry per
 * request even when several completions are already sitting in the ring, which
 * throws away most of what batching buys. This publishes anything queued, then
 * drains the completion ring until it is empty, sleeping only when there is
 * genuinely nothing to reap. Returns 0, or -1 if a request never completed.
 */
int fasync_wait_all(void);

/* Resolve this handle, waiting if necessary. Returns the raw syscall result
 * (bytes transferred for read/write, 0 for fsync/openat-style success) or
 * -errno. */
long fasync_result(fasync_id id);

/* ------------------------------------------------------------------ */
/* Provenance.                                                         */
/* ------------------------------------------------------------------ */

/* The tag inherited by a derived pointer. `offset` is the byte offset of the
 * derived pointer within the request's result. */
struct fasync_prov {
  fasync_id req;
  unsigned long offset;
  unsigned long len;
};

/*
 * Recover the provenance of a pointer, if it was derived from an async result
 * and has not resolved yet. Returns 1 and fills `out` if the pointer lies
 * inside a pending request's range, 0 otherwise.
 */
int fasync_provenance(const void* ptr, size_t size, struct fasync_prov* out);

/* Derive a sub-range of a pending result, inheriting its provenance. Returns a
 * pointer to the derived range. Unsafe to dereference while pending -- use
 * FASYNC_ACCESS() first, exactly as for the parent. */
void* fasync_derive(void* base, unsigned long offset, size_t len,
                    struct fasync_prov* out);

/* ------------------------------------------------------------------ */
/* Observability.                                                      */
/*                                                                     */
/* These counters are the evidence for the design's performance claims:  */
/* in particular kernel_wait_entries counts the context switches that      */
/* were actually taken, versus spins/cq_polls which were served entirely   */
/* from userspace.                                                         */
/* ------------------------------------------------------------------ */

struct fasync_stats {
  unsigned long sqes_queued;         /* SQEs written into the SQ ring      */
  unsigned long kernel_submit_entries; /* non-blocking enters (batch submit) */
  unsigned long kernel_wait_entries; /* enters that were allowed to block  */
  unsigned long userspace_cq_polls;  /* CQ reads served with no syscall    */
  unsigned long resolve_calls;       /* entries into the resolve slow path */
  unsigned long fast_path_hits;      /* resolve calls that checked one flag */
  unsigned long spin_rounds;         /* userspace spin iterations          */
  unsigned long parks;               /* times we actually slept            */
  unsigned long completions_reaped;  /* CQEs consumed                      */
};

void fasync_get_stats(struct fasync_stats* out);
void fasync_reset_stats(void);

/* Description of the most recent runtime failure, or "" if none. The runtime
 * reports failures through return values only, so that it does not depend on
 * stdio (whose symbols resolve from a different archive). */
const char* fasync_last_error(void);

#endif /* FASYNC_H */
