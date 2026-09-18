/* fasync_internal.h -- private API shared between the runtime's own TUs. */
#pragma once

#include "fasync.h"
#include "fasync_shared.h"

fasync_id fasync_push_sqe(unsigned char op, int fd, unsigned long addr,
                          unsigned int len, unsigned long offset,
                          void* result_buf, size_t result_len,
                          unsigned char sqe_flags);

fasync_id fasync_push_buf(unsigned char op, int fd, void* buf, size_t len,
                          unsigned long offset, unsigned char sqe_flags);

struct fasync_req_shared* fasync_req_lookup(fasync_id id);

void fasync_req_release(struct fasync_req_shared* r);

/* Wait without releasing so the same handle stays resolvable. */
long fasync_req_wait(struct fasync_req_shared* r);