#include <stdfil.h>
#include <pizlonated_syscalls.h>

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include "fasync.h"
#include "fasync_io_uring.h"
#include "fasync_syscalls.h"
#include "fasync_shared.h"
#include "fasync_internal.h"
#include "filc_async_runtime.h"

/* pending opens return negative handle from -2 */
#define FASYNC_MAX_PENDING_FDS 64

struct fasync_pending_fd {
  int used;
  int resolved;
  fasync_id id; /* the request that will produce the fd */
  long result;  /* once resolved the fd or the open's -errno */
};

static struct fasync_pending_fd g_pending_fds[FASYNC_MAX_PENDING_FDS];

static int fasync_pending_fd_new(fasync_id id) {
  for (int i = 0; i < FASYNC_MAX_PENDING_FDS; i++) {
    if (g_pending_fds[i].used)
      continue;
    g_pending_fds[i].used = 1;
    g_pending_fds[i].resolved = 0;
    g_pending_fds[i].id = id;
    return -(i + 2); /* -1 stays the failure value */
  }
  return -1;
}

static struct fasync_pending_fd* fasync_pending_fd_lookup(int fd) {
  /* bounds checked before negating so INT_MIN cannot overflow */
  if (fd >= -1 || fd < -(FASYNC_MAX_PENDING_FDS + 1))
    return 0;
  struct fasync_pending_fd* p = &g_pending_fds[-fd - 2];
  return p->used ? p : 0;
}

fasync_id fasync_pread(int fd, void* buf, size_t len, unsigned long offset) {
  fasync_lock();
  fasync_id id = fasync_do_pread(fd, buf, len, offset);
  if (id)
    fasync_track_read(id, buf);
  fasync_unlock();
  return id;
}

/* The kernel reads the source when the request runs, so a call still
 * producing it is waited for first, outside the runtime's lock. */
fasync_id fasync_pwrite(int fd, void* buf, size_t len, unsigned long offset) {
  filc_async_wait_buffer(NULL, buf);
  fasync_lock();
  fasync_id id = fasync_do_pwrite(fd, buf, len, offset);
  fasync_unlock();
  return id;
}

/* Likewise for the path, which the kernel reads later. */
fasync_id fasync_openat(int dirfd, const char* path, int flags, int mode) {
  filc_async_wait_buffer(NULL, path);
  fasync_lock();
  fasync_id id = fasync_do_openat(dirfd, path, flags, mode);
  fasync_unlock();
  return id;
}

fasync_id fasync_do_pread(int fd, void* buf, size_t len, unsigned long offset) {
  long real = fasync_fd_resolve(fd); /* may wait on a pending open */
  if (real < 0) {
    errno = (int)-real;
    return 0;
  }
  fd = (int)real;

  zcheck(buf, len); /* in bounds and writable */
  return fasync_push_buf(FASYNC_OP_READ, fd, buf, len, offset, 0);
}

fasync_id fasync_do_pwrite(int fd, void* buf, size_t len, unsigned long offset) {
  long real = fasync_fd_resolve(fd);
  if (real < 0) {
    errno = (int)-real;
    return 0;
  }
  fd = (int)real;

  zcheck_readonly(buf, len); /* the kernel only reads it */

  return fasync_push_sqe(FASYNC_OP_WRITE, fd, (unsigned long)(size_t)buf, len,
                         offset, 0, 0, 0);
}

fasync_id fasync_fsync(int fd) {
  fasync_lock();
  long real = fasync_fd_resolve(fd);
  fasync_id id = 0;
  if (real < 0)
    errno = (int)-real;
  else
    id = fasync_push_sqe(FASYNC_OP_FSYNC, (int)real, 0, 0, 0, 0, 0, 0);
  fasync_unlock();
  return id;
}

static fasync_id fasync_close_locked(int fd);

fasync_id fasync_close(int fd) {
  fasync_lock();
  fasync_id id = fasync_close_locked(fd);
  fasync_unlock();
  return id;
}

static fasync_id fasync_close_locked(int fd) {
  struct fasync_pending_fd* p = fasync_pending_fd_lookup(fd);
  long real = fasync_fd_resolve(fd);
  if (real < 0) {
    /* a pending open that failed has no fd to close but its handle goes */
    if (p)
      p->used = 0;
    errno = (int)-real;
    return 0;
  }
  fasync_id id = fasync_push_sqe(FASYNC_OP_CLOSE, (int)real, 0, 0, 0, 0, 0, 0);

  /* a closed pending handle frees its slot like a closed fd */
  if (id && p)
    p->used = 0;
  return id;
}

int fasync_open_pending(int dirfd, const char* path, int flags, int mode) {
  fasync_id id = fasync_openat(dirfd, path, flags, mode);
  if (!id)
    return -1;
  fasync_lock();
  int handle = fasync_pending_fd_new(id);
  fasync_unlock();
  return handle;
}

fasync_id fasync_do_openat(int dirfd, const char* path, int flags, int mode) {
  if (!path) {
    errno = EFAULT;
    return 0;
  }
  /* The kernel has no Fil-C capability. Prove the NUL terminator is inside
   * the source object's bounds before lending the pointer to io_uring. */
  uintptr_t start = (uintptr_t)path;
  uintptr_t upper = (uintptr_t)zgetupper((void*)path);
  if (upper <= start) {
    errno = EFAULT;
    return 0;
  }
  size_t available = upper - start;
  if (available > 4096)
    available = 4096;
  zcheck_readonly((void*)path, available);

  size_t i = 0;
  while (i < available && path[i])
    i++;
  if (i == available) {
    errno = ENAMETOOLONG;
    return 0;
  }

  /* path rides in addr mode in len */
  return fasync_push_sqe(FASYNC_OP_OPENAT, dirfd, (unsigned long)(size_t)path,
                         (unsigned int)mode, (unsigned long)flags, 0, 0, 0);
}

int fasync_ready(fasync_id id) {
  fasync_lock();
  struct fasync_req_shared* r = fasync_req_lookup(id);
  int ready = 0;
  if (r) {
    if (r->state == FASYNC_REQ_PENDING)
      fasync_poll();
    ready = r->state != FASYNC_REQ_PENDING;
  }
  fasync_unlock();
  return ready;
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
  fasync_lock();
  struct fasync_req_shared* r = fasync_req_lookup(id);
  if (!r) {
    fasync_unlock();
    return -EINVAL;
  }

  long result = fasync_req_wait(r);
  /* an explicit read's buffer resolves with it */
  if (r->task) {
    void* task = r->task;
    r->task = 0;
    filc_async_complete(task, result);
  }
  fasync_req_release(r);
  fasync_unlock();
  return result;
}

long fasync_fd_resolve(int fd) {
  if (fd >= 0)
    return fd;

  struct fasync_pending_fd* p = fasync_pending_fd_lookup(fd);
  if (!p)
    return -EBADF; /* -1 is the reserved failure value not a handle */

  /* fasync_result releases the request so the outcome is kept here */
  if (!p->resolved) {
    p->result = fasync_result(p->id);
    p->resolved = 1;
  }
  return p->result;
}
