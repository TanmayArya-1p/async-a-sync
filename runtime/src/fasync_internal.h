/*
 * fasync_internal.h -- private API shared between the runtime's own TUs.
 * Nothing here is for programs; the public surface is fasync.h / fasync_dep.h.
 */
#pragma once

#include "fasync.h"
#include "fasync_shared.h"

/* Queue one SQE and return a handle without publishing or waiting (core). */
fasync_id fasync_push_sqe(unsigned char op, int fd, unsigned long addr,
                          unsigned int len, unsigned long offset,
                          void* result_buf, size_t result_len,
                          unsigned char sqe_flags);

/* push_sqe with the SQE operands equal to the result buffer (core). */
fasync_id fasync_push_buf(unsigned char op, int fd, void* buf, size_t len,
                          unsigned long offset, unsigned char sqe_flags);

/* Handle -> request, or 0 when the slot was recycled/released (core). */
struct fasync_req_shared* fasync_req_lookup(fasync_id id);

/* Return a request's slot to the free list (core). */
void fasync_req_release(struct fasync_req_shared* r);

/* Wait for a request to reach a terminal state WITHOUT releasing it, so the
 * same handle stays resolvable afterwards (fasync_syscalls.c). */
long fasync_req_wait(struct fasync_req_shared* r);