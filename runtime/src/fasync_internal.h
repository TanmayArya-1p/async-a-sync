#pragma once

#include "fasync.h"
#include "fasync_shared.h"

fasync_id fasync_push_sqe(unsigned char op, int fd, unsigned long addr,
                          size_t len, unsigned long offset,
                          void* result_buf, size_t result_len,
                          unsigned char sqe_flags);

fasync_id fasync_push_buf(unsigned char op, int fd, void* buf, size_t len,
                          unsigned long offset, unsigned char sqe_flags);

struct fasync_req_shared* fasync_req_lookup(fasync_id id);

void fasync_req_release(struct fasync_req_shared* r);

/* wait without releasing so handle stays resolvable */
long fasync_req_wait(struct fasync_req_shared* r);

/* One recursive lock over the ring, the request table and the pending-fd
 * table, so any thread may issue requests and wait on them, one at a time.
 * Never held while waiting for the async framework or running a body: the
 * framework's calls into this runtime take it themselves. */
void fasync_lock(void);
void fasync_unlock(void);

/* The requests behind the fasync_* calls, without their bookkeeping for the
 * async framework: the io_uring runtime issues annotated calls with these.
 * Callers hold fasync_lock, and have already waited, through the framework,
 * for any call still producing a pwrite source or an openat path. */
fasync_id fasync_do_pread(int fd, void* buf, size_t len, unsigned long offset);
fasync_id fasync_do_pwrite(int fd, void* buf, size_t len, unsigned long offset);
fasync_id fasync_do_openat(int dirfd, const char* path, int flags, int mode);

/* Gives an explicit fasync_pread a task of its own and marks its buffer
 * pending for it (filc_async_uring.c). */
void fasync_track_read(fasync_id id, void* buf);
