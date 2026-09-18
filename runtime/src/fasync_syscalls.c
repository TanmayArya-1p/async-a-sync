#include <stdfil.h>
#include <pizlonated_syscalls.h>

#include <errno.h>
#include <string.h>

#include "fasync.h"
#include "fasync_io_uring.h"
#include "fasync_syscalls.h"
#include "fasync_shared.h"
#include "fasync_internal.h"

/* pending opens return negative handle from -2 */
#define FASYNC_MAX_PENDING_FDS 64

struct fasync_pending_fd {
  int used;
  fasync_id id; /* the request that will produce the fd */
  long fd;      /* resolved value or -1 while still pending */
};

static struct fasync_pending_fd g_pending_fds[FASYNC_MAX_PENDING_FDS];

static int fasync_pending_fd_new(fasync_id id) {
  for (int i = 0; i < FASYNC_MAX_PENDING_FDS; i++) {
    if (g_pending_fds[i].used)
      continue;
    g_pending_fds[i].used = 1;
    g_pending_fds[i].id = id;
    g_pending_fds[i].fd = -1;
    return -(i + 2); /* -1 stays the failure value */
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

  /* kernel reads the source at execute time */
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
  /* paths have no length so check terminator byte */
  zcheck_readonly((void*)path, 1);

  /* path rides in addr mode in len */
  return fasync_push_sqe(FASYNC_OP_OPENAT, dirfd, (unsigned long)(size_t)path,
                         (unsigned int)mode, (unsigned long)flags, 0, 0, 0);
}

int fasync_ready(fasync_id id) {
  struct fasync_req_shared* r = fasync_req_lookup(id);
  if (!r)
    return 0;
  if (r->state == FASYNC_REQ_PENDING)
    fasync_poll();
  return r->state != FASYNC_REQ_PENDING;
}

/* wait without releasing so the handle stays resolvable */
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

long fasync_fd_resolve(int fd) {
  if (fd >= 0)
    return fd;

  if (fd == -1)
    return -EBADF; /* reserved failure value not a handle */
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
