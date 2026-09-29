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

/* The runtime is single-threaded: one io_uring ring and one request table,
 * with no locks. The thread that sets up the ring owns it; requests and waits
 * from any other thread abort here. A no-op until the ring exists. */
void fasync_check_thread(void);

/* nonzero when the caller is not the ring's owner */
int fasync_foreign_thread(void);

/* The requests behind the fasync_* calls, without their bookkeeping for the
 * async framework: the io_uring runtime issues annotated calls with these.
 * `task` is the call that issues them, which a source or path buffer never
 * waits for; NULL for none. */
fasync_id fasync_do_pread(int fd, void* buf, size_t len, unsigned long offset);
fasync_id fasync_do_pwrite(void* task, int fd, void* buf, size_t len,
                           unsigned long offset);
fasync_id fasync_do_openat(void* task, int dirfd, const char* path, int flags,
                           int mode);

/* Gives an explicit fasync_pread a task of its own and marks its buffer
 * pending for it (filc_async_uring.c). */
void fasync_track_read(fasync_id id, void* buf);
