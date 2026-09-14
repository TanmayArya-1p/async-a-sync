/*
 * fasync_dep.c -- dependency construction from declared effect sets.
 *
 * See fasync_dep.h for the design rationale. The short version: an operation
 * declares what it reads and writes, and the dependency DAG falls out of those
 * declarations. Before falling back to the declarations, though, the analyser
 * tries to *prove* two operands disjoint using Fil-C's capability bounds, which
 * is what keeps the annotation burden low.
 *
 * Compiled by filcc, so it runs under the full capability model -- including
 * zgetlower/zgetupper, which is precisely the machinery the disjointness proof
 * is built on.
 */

#include <stdfil.h>

#include <stdlib.h>
#include <string.h>

#include "fasync_dep.h"

/* ------------------------------------------------------------------ */
/* Ranges                                                              */
/* ------------------------------------------------------------------ */

struct fasync_range {
  const char* lo;
  const char* hi;
};

/*
 * The true extent of the object a pointer points into.
 *
 * This is the capability doing real work: Fil-C stores bounds out-of-band from
 * the pointer's bits, so the analyser can recover them for a pointer that a
 * declaration only described loosely. `len` widens the range if the declared
 * access extends past the object, which keeps the proof conservative.
 */
static struct fasync_range fasync_capability_range(const void* buf, size_t len) {
  struct fasync_range r;
  const char* lower = (const char*)zgetlower((void*)buf);
  const char* upper = (const char*)zgetupper((void*)buf);
  const char* end = (const char*)buf + len;

  r.lo = buf < (const void*)lower ? (const char*)buf : lower;
  r.hi = end > upper ? end : upper;
  return r;
}

/* Two half-open ranges are disjoint iff one ends before the other begins. */
static int fasync_ranges_disjoint(const struct fasync_range* a,
                                  const struct fasync_range* b) {
  return a->hi <= b->lo || b->hi <= a->lo;
}

/* Does this access kind write? */
static int fasync_kind_writes(unsigned kind) {
  return kind == FASYNC_OUT || kind == FASYNC_INOUT;
}

/* Does this access kind read? */
static int fasync_kind_reads(unsigned kind) {
  return kind == FASYNC_IN || kind == FASYNC_INOUT;
}

