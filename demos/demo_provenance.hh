/*
 * demo_provenance.hh -- the crux: two calls on two *different* descriptors of
 * one file, ordered by a token:
 *
 *     fasync_tracker* prov = fasync_tracker_new();
 *     fasync_tagged_pwrite(fdw, b1, len, 0, prov, FASYNC_INOUT);
 *     fasync_tagged_pread (fdr, b2, len, 0, prov, FASYNC_INOUT);
 *
 * Nothing marks this pair for the runtime: the buffers are separate, and so
 * are the descriptors -- neither is a pending open, no value flows between the
 * calls, and the only thing they share is the file behind two fd values, which
 * an int cannot carry. Returns the number of claims verified (0..2): untagged,
 * nothing holds the read and the writer is still queued when it is issued;
 * tagged, the reader cannot be issued until the writer has finished, so it
 * sees exactly the author's bytes.
 *
 * The buffers come from demo_prov_setup() in utils.hh; the run file seeds the
 * file, opens it twice, and reports.
 */
#pragma once

#include <string.h>

#include "fasync_dep.h"
#include "utils.hh"

static int demo_provenance(int fdw, int fdr) {
  int verified = 0;

  /* No token: a write on one descriptor and a read on another share nothing. */
  fasync_id w = fasync_pwrite(fdw, demo_prov_author, demo_prov_len, 0);
  fasync_id r = fasync_pread(fdr, demo_prov_plain_reader, demo_prov_len, 0);
  if (w && r && !fasync_ready(w) && !fasync_ready(r) &&
      fasync_result(w) == (long)demo_prov_len &&
      fasync_result(r) == (long)demo_prov_len)
    verified++;

  if (demo_prov_resterile(fdw) < 0)
    return verified;

  /* One token for both calls: f2's issue waited until f1 had finished. */
  fasync_tracker* tag = fasync_tracker_new();
  if (!tag)
    return verified;
  w = fasync_tagged_pwrite(fdw, demo_prov_author, demo_prov_len, 0, tag,
                           FASYNC_INOUT);
  r = fasync_tagged_pread(fdr, demo_prov_tagged_reader, demo_prov_len, 0, tag,
                          FASYNC_INOUT);
  if (w && r && fasync_ready(w) &&
      fasync_result(w) == (long)demo_prov_len &&
      fasync_result(r) == (long)demo_prov_len &&
      memcmp(demo_prov_author, demo_prov_tagged_reader, demo_prov_len) == 0)
    verified++;

  fasync_tracker_free(tag);
  return verified;
}