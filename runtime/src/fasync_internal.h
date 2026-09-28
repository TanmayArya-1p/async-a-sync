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

/* nonzero when the caller is not the ring's owner (resolution from other
 * threads is a no-op, like the compiler's access hook) */
int fasync_foreign_thread(void);
