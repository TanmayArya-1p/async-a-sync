/*
 * fasync_token.c -- serialization-token-ordered syscalls (Fil-C, memory-safe).
 *
 * The token of async-a-sync.pdf, implemented where the dependency actually
 * bites: at syscall issue. fasync_tagged_pread / fasync_tagged_pwrite take the
 * token the calls share, and a call is not enqueued until every *conflicting*
 * tagged call already issued on that token has completed. "Conflicting" means
 * at least one side writes, so reads overlap freely. The queue lives in the
 * fasync_tracker ABI (fasync_dep.h); both ops share one token.
 */

#include <stdfil.h>
#include <pizlonated_syscalls.h>

#include <string.h>

#include "fasync.h"
#include "fasync_dep.h"
#include "fasync_shared.h"
#include "fasync_internal.h"

static int fasync_token_conflicts(unsigned a, unsigned b) {
  return a == FASYNC_OUT || a == FASYNC_INOUT || b == FASYNC_OUT ||
         b == FASYNC_INOUT;
}

/* Wait until every conflicting tagged call already issued on `tok` has
 * finished, then compact the queue down to the calls that still matter.
 * A completed slot (resolved by a conflict, or via fasync_ready/result on its
 * own) can no longer order anything and is dropped; a still-pending slot that
 * does not conflict with the new call survives because a *later* writing call
 * must wait on it. */
static void fasync_token_wait(fasync_tracker* tok, unsigned kind) {
  if (!tok || !tok->n)
    return;

  unsigned keep = 0;
  for (unsigned i = 0; i < tok->n; i++) {
    struct fasync_req_shared* r = fasync_req_lookup(tok->q[i].id);
    if (r && r->state == FASYNC_REQ_PENDING &&
        fasync_token_conflicts(tok->q[i].kind, kind))
      fasync_req_wait(r);
    r = fasync_req_lookup(tok->q[i].id);
    if (r && r->state == FASYNC_REQ_PENDING)
      tok->q[keep++] = tok->q[i];
  }
  tok->n = keep;
}

/* Remember that `id` was issued on `tok` so a later conflicting tagged call
 * waits on it. A full queue is relieved by waiting the oldest slot out and
 * shifting, which honours its ordering rather than silently dropping it. */
static void fasync_token_track(fasync_tracker* tok, fasync_id id,
                               unsigned kind) {
  if (!tok || !id)
    return;

  while (tok->n >= FASYNC_TOKEN_QLEN) {
    struct fasync_req_shared* r = fasync_req_lookup(tok->q[0].id);
    if (r && r->state == FASYNC_REQ_PENDING)
      fasync_req_wait(r);
    for (unsigned i = 1; i < tok->n; i++)
      tok->q[i - 1] = tok->q[i];
    tok->n--;
  }

  tok->q[tok->n].id = id;
  tok->q[tok->n].kind = kind;
  tok->n++;
}

fasync_id fasync_tagged_pread(int fd, void* buf, size_t len, unsigned long offset,
                              fasync_tracker* tok, unsigned kind) {
  fasync_token_wait(tok, kind);
  fasync_id id = fasync_pread(fd, buf, len, offset);
  fasync_token_track(tok, id, kind);
  return id;
}

fasync_id fasync_tagged_pwrite(int fd, void* buf, size_t len, unsigned long offset,
                               fasync_tracker* tok, unsigned kind) {
  fasync_token_wait(tok, kind);
  fasync_id id = fasync_pwrite(fd, buf, len, offset);
  fasync_token_track(tok, id, kind);
  return id;
}