/*
 * fasync_syscalls.c -- the public async syscall surface (Fil-C, memory-safe).
 *
 * Split out of fasync.c: the pending-descriptor slot table and the calls that
 * enqueue work and report on it. The ring plumbing and request table live in
 * fasync.c; token-ordered variants live in fasync_token.c.
 *
 * Whatever the caller asked for is validated against the capability model here
 * (zcheck/zcheck_readonly) before the kernel is allowed near it -- the async
 * path does not get to skip the check the synchronous zsys_* path does, it just
 * moves the waiting elsewhere.
 */

#include <stdfil.h>
#include <pizlonated_syscalls.h>

#include <errno.h>
#include <string.h>

#include "fasync.h"
#include "fasync_io_uring.h"
#include "fasync_syscalls.h"
#include "fasync_shared.h"
#include "fasync_internal.h"

/* Provenance for descriptors: a pending open returns a negative *handle* that
 * every fd-taking op resolves first. Handles start at -2 so -1 stays an
 * unambiguous failure value (letting slot 0 alias it was a real bug). */
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
    return -(i + 2); /* -(index + 2), so -1 stays the failure value */
  }
  return -1;
}

fasync_id fasync_pread(int fd, void* buf, size_t len, unsigned long offset) {
  long real = fasync_fd_resolve(fd); /* may wait on a pending open */
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

  zcheck_readonly(buf, len); /* the kernel only reads it */

  /* The source must be resolved *now*: the kernel reads these bytes when it
   * executes the SQE, and there is no way to tell it "use whatever lands here".
   * So a write's source is a genuine sync point, resolved eagerly rather than
   * on a later access -- which costs the overlap with the read that feeds it. */
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

/* Open, returning a *pending descriptor*: a negative handle usable anywhere an
 * fd is accepted, resolved lazily by the first op that takes it. */
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
  /* Paths have no length argument, so the check is for the terminator byte;
   * the runtime's own string handling rejects a run off the allocation end. */
  zcheck_readonly((void*)path, 1);

  /* sqe->len carries the mode, sqe->addr the path; the op yields an fd, so
   * there is no result buffer to track. */
  return fasync_push_sqe(FASYNC_OP_OPENAT, dirfd, (unsigned long)(size_t)path,
                         (unsigned int)mode, (unsigned long)flags, 0, 0, 0);
}

/* fasync_openat_direct -- openat into an explicit direct-descriptor slot so a
 * dependent op can be submitted against an fd that does not exist yet -- is
 * deliberately absent: it is only meaningful with a registered file table, and
 * linux ignores an explicit slot unless one exists (see stage6b_fd_chain_probe).
 * It lands with the pipelining work. */

int fasync_ready(fasync_id id) {
  struct fasync_req_shared* r = fasync_req_lookup(id);
  if (!r)
    return 0;
  if (r->state == FASYNC_REQ_PENDING)
    fasync_poll();
  return r->state != FASYNC_REQ_PENDING;
}

/* Wait for one request to reach a terminal state WITHOUT releasing it, so the
 * caller keeps a resolvable handle. This is fasync_result's body separated out
 * for the token section, which must block on a tagged predecessor while keeping
 * it resolvable later. A still-queued request is published first: waiting on an
 * SQE never handed to the kernel would wait forever, and nothing forces an
 * explicit fasync_submit(). */
long fasync_req_wait(struct fasync_req_shared* r) {
  if (r->state == FASYNC_REQ_PENDING)
    fasync_submit();

  while (r->state == FASYNC_REQ_PENDING) {
    fasync_poll();
    if (r->state == FASYNC_REQ_PENDING)
      fasync_block();
  }
  return r->result;
}

long fasync_result(fasync_id id) {
  struct fasync_req_shared* r = fasync_req_lookup(id);
  if (!r)
    return -EINVAL;

  long result = fasync_req_wait(r);
  fasync_req_release(r);
  return result;
}

/* Resolve a descriptor: a real fd passes straight through, a pending handle is
 * waited on and replaced by the fd it produced. This is the whole of fd
 * provenance on the consumer side, and why ops can be written against a
 * descriptor that does not exist yet. */
long fasync_fd_resolve(int fd) {
  if (fd >= 0)
    return fd;

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