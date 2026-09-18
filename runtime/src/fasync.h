/*
 * fasync.h -- implicit futures for syscalls, on top of Fil-C + io_uring.
 *
 * The public surface of the runtime extension: everything programs link
 * against, and the one symbol the FilPizlonator patch needs
 * (filc_resolve_pending). The three mechanisms -- zero-context-switch
 * submission, provenance-tracked results, lazy resolution on first genuine
 * access -- and the rationale behind the visible macro below are in
 * docs/RUNTIME.md.
 */
#pragma once

#include <stddef.h>

/* A handle for one in-flight async syscall: the tag provenance tracking is
 * built on. It names the request, not the buffer. */
typedef unsigned long fasync_id;

/* The resolution logic, used by programs calling FASYNC_ACCESS(). */
void* fasync_resolve_pending(void* ptr, size_t size);

/*
 * The hook the patched compiler emits calls to.
 *
 * Two symbols exist for this one operation because of Fil-C's naming
 * convention: a name starting with filc_ is referenced *unprefixed* (the native
 * bridge in fasync_native.c), whereas an ordinary external called from
 * filcc-compiled code is referenced as pizlonated_<name>. Hence the macro below
 * reaches fasync_resolve_pending while the pass calls filc_resolve_pending.
 */
void* filc_resolve_pending(void* ptr, size_t size);

/* The access points the patched compiler inserts a resolution check on. With an
 * unpatched compiler these are explicit calls; with a patched one they are
 * redundant (the compiler already emitted its own hook) and this is a no-op. */
#ifdef FASYNC_COMPILER_INSERTS_CHECKS
#define FASYNC_ACCESS(ptr, size) ((void)0)
#else
#define FASYNC_ACCESS(ptr, size) ((void)fasync_resolve_pending((void*)(ptr), (size)))
#endif

/* Effect-set kinds, for the dependency layer (fasync_dep.h). */
enum fasync_access_kind {
  FASYNC_IN = 0,   /* operation reads this range   */
  FASYNC_OUT = 1,  /* operation writes this range  */
  FASYNC_INOUT = 2 /* operation may do both        */
};

/* Fixed-size ABI types (the ring layout must not drift). */
typedef unsigned int fasync_uint32;

/* Async syscalls: each returns a handle immediately; nothing blocks at the call
 * site. There is deliberately no fasync_read() -- a plain read(2) has no offset
 * and would depend on the file cursor at execution time, not submission time. */
fasync_id fasync_pread(int fd, void* buf, size_t len, unsigned long offset);
fasync_id fasync_pwrite(int fd, void* buf, size_t len, unsigned long offset);
fasync_id fasync_fsync(int fd);
fasync_id fasync_close(int fd);
fasync_id fasync_openat(int dirfd, const char* path, int flags, int mode);

/* Descriptor provenance: a descriptor produced by an async open does not exist
 * until that open completes, so fasync_open_pending returns a *pending*
 * descriptor -- a negative value, since a real descriptor never is -- usable
 * anywhere an fd is accepted. Every op resolves it first, so using one is what
 * creates the dependency. */
int fasync_open_pending(int dirfd, const char* path, int flags, int mode);

/* Resolve a descriptor: a real fd passes through, a pending one is waited on.
 * Returns the fd, or -errno. */
long fasync_fd_resolve(int fd);

/* fasync_openat_direct -- openat into an explicit direct-descriptor slot so a
 * dependent op can be submitted against an fd that does not exist yet -- is
 * deliberately absent; it arrives with the promise-pipelining work. */

/* Publish all queued SQEs to the kernel. Non-blocking. Returns the number
 * submitted, or -1 on error. */
int fasync_submit(void);

/* Has this request's completion landed? Never blocks. */
int fasync_ready(fasync_id id);

/* Wait until nothing is in flight, draining the completion ring and sleeping
 * only when there is genuinely nothing to reap. Returns 0, or -1 if a request
 * never completed. */
int fasync_wait_all(void);

/* Resolve this handle, waiting if necessary. Returns the raw syscall result
 * (bytes transferred, or 0 for fsync/openat-style success) or -errno. */
long fasync_result(fasync_id id);

/* Provenance. */
/* The tag inherited by a derived pointer (`offset` within the request's result). */
struct fasync_prov {
  fasync_id req;
  unsigned long offset;
  unsigned long len;
};

/* Recover the provenance of a pointer, if it was derived from an async result
 * and has not resolved yet. 1 and fills `out` when the pointer lies inside a
 * pending request's range, 0 otherwise. */
int fasync_provenance(const void* ptr, size_t size, struct fasync_prov* out);

/* Derive a sub-range of a pending result, inheriting its provenance. Unsafe to
 * dereference while pending -- use FASYNC_ACCESS() first. */
void* fasync_derive(void* base, unsigned long offset, size_t len,
                    struct fasync_prov* out);

/* Observability: the counters that back the design's performance claims --
 * kernel_wait_entries counts context switches actually taken, versus spins and
 * CQ polls served entirely from userspace. */
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
  unsigned long memo_hits;           /* slow-path hits answered from the
                                      * negative-range memo without a walk */
};

void fasync_get_stats(struct fasync_stats* out);
void fasync_reset_stats(void);

/* Description of the most recent runtime failure, or "". The runtime reports
 * failures through return values only, so it does not depend on which archive
 * member resolves stdio first. */
const char* fasync_last_error(void);