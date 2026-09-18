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

/* wait conflicting calls drop completed slots */
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

/* full queue waits oldest slot for room */
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
