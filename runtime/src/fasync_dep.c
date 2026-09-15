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

/* ------------------------------------------------------------------ */
/* Conflict analysis                                                   */
/* ------------------------------------------------------------------ */

/*
 * Classify a single access pair.
 *
 * Returns 0 for no conflict, or one of the edge kinds (1..3) when `b` must
 * follow `a`:
 *
 *   1  a writes, b reads    (true dependency)
 *   2  a reads, b writes    (anti-dependency)
 *   3  a writes, b writes   (output dependency)
 *
 * Reads against reads never conflict, which is what makes reader-parallel
 * workloads stay parallel.
 */
static int fasync_access_conflict(const struct fasync_access* a,
                                 const struct fasync_access* b,
                                 int* auto_disjoint) {
  /*
   * Resource accesses first. Two operations touching *different* resources are
   * independent no matter what kinds they declare -- the same kind of proof the
   * capability ranges give below, but for things that have no address.
   */
  if (!a->buf || !b->buf) {
    if (a->buf || b->buf)
      return 0; /* a resource access and a buffer access are unrelated */
    if (a->resource != b->resource) {
      if (auto_disjoint)
        *auto_disjoint = 1;
      return 0;
    }
  } else {
    struct fasync_range ra = fasync_capability_range(a->buf, a->len);
    struct fasync_range rb = fasync_capability_range(b->buf, b->len);

    if (fasync_ranges_disjoint(&ra, &rb)) {
    /*
     * The declared kinds may well have conflicted, but the ranges provably do
     * not overlap -- so no edge is needed. Count it, because this is annotation
     * the programmer did not have to write.
     */
      if (auto_disjoint)
        *auto_disjoint = 1;
      return 0;
    }
  }

  int a_writes = fasync_kind_writes(a->kind);
  int a_reads = fasync_kind_reads(a->kind);
  int b_writes = fasync_kind_writes(b->kind);
  int b_reads = fasync_kind_reads(b->kind);

  if (a_writes && b_writes)
    return 3;
  if (a_writes && b_reads)
    return 1;
  if (a_reads && b_writes)
    return 2;
  return 0;
}

/*
 * Do operations `a` and `b` conflict?
 *
 * The strongest conflict across all access pairs wins, and `*auto_disjoint` is
 * set only when every pair that could have conflicted was dissolved by the
 * range proof.
 */
int fasync_ops_conflict(const struct fasync_op* a, const struct fasync_op* b,
                        int* auto_disjoint) {
  int strongest = 0;
  int any_dissolved = 0;
  int any_declared_conflict = 0;

  for (unsigned i = 0; i < a->n_accesses; i++) {
    for (unsigned j = 0; j < b->n_accesses; j++) {
      const struct fasync_access* aa = &a->accesses[i];
      const struct fasync_access* bb = &b->accesses[j];

      /* Would this pair have conflicted if we ignored the ranges? */
      int declared =
          (fasync_kind_writes(aa->kind) &&
           (fasync_kind_writes(bb->kind) || fasync_kind_reads(bb->kind))) ||
          (fasync_kind_reads(aa->kind) && fasync_kind_writes(bb->kind));

      int dissolved = 0;
      int edge = fasync_access_conflict(aa, bb, &dissolved);

      if (edge)
        strongest = edge;
      if (declared && !edge) {
        any_dissolved = 1;
      }
      if (declared && edge)
        any_declared_conflict = 1;
    }
  }

  if (auto_disjoint)
    *auto_disjoint = (any_dissolved && !any_declared_conflict) ? 1 : 0;

  return strongest != 0;
}

/* ------------------------------------------------------------------ */
/* DAG construction                                                    */
/* ------------------------------------------------------------------ */

unsigned fasync_build_dag(const struct fasync_op* ops, unsigned n_ops,
                          unsigned* edges, unsigned max_edges,
                          struct fasync_dag_stats* stats) {
  struct fasync_dag_stats local;
  memset(&local, 0, sizeof(local));
  local.ops = n_ops;

  unsigned n_edges = 0;

  /* O(n^2) over operations: fine for a syscall batch, and it makes the
   * dependency structure explicit rather than hidden behind an index. */
  for (unsigned i = 0; i < n_ops; i++) {
    for (unsigned j = i + 1; j < n_ops; j++) {
      int auto_disjoint = 0;
      int conflict = fasync_ops_conflict(&ops[i], &ops[j], &auto_disjoint);

      /* Count the saving BEFORE bailing out: a dissolved conflict produces no
       * edge, so it would otherwise never be recorded. */
      if (auto_disjoint)
        local.auto_disjoint_pairs++;

      if (!conflict)
        continue;

      if (n_edges >= max_edges) {
        if (stats)
          *stats = local;
        return 0;
      }

      edges[n_edges++] = i * n_ops + j;
      local.edges++;
      local.declared_edges++;

      /* Classify the dominant edge kind for reporting. */
      int kind = 0;
      for (unsigned x = 0; x < ops[i].n_accesses && !kind; x++) {
        for (unsigned y = 0; y < ops[j].n_accesses && !kind; y++) {
          int dissolved = 0;
          kind = fasync_access_conflict(&ops[i].accesses[x],
                                        &ops[j].accesses[y], &dissolved);
        }
      }
      if (kind == 1)
        local.read_write_edges++;
      else if (kind == 2)
        local.write_read_edges++;
      else if (kind == 3)
        local.write_write_edges++;
    }
  }

  if (stats)
    *stats = local;
  return n_edges;
}

/* ------------------------------------------------------------------ */
/* Serialization tokens (async-a-sync.pdf)                             */
/* ------------------------------------------------------------------ */

/*
 * A token is just a named resource that everyone sharing it claims INOUT. That
 * is the whole implementation, which is the point: async-a-sync.pdf presents the
 * token as its own mechanism, but it is a special case of a declared effect set
 * over a non-address resource.
 */
static unsigned long fasync_next_resource = 1;

fasync_tracker* fasync_tracker_new(void) {
  fasync_tracker* t = malloc(sizeof(fasync_tracker));
  if (!t)
    return 0;
  t->resource = fasync_next_resource++;
  return t;
}

void fasync_tracker_free(fasync_tracker* tracker) { free(tracker); }

struct fasync_access fasync_tracker_access(const fasync_tracker* tracker,
                                           unsigned kind) {
  struct fasync_access a;
  a.buf = 0;
  a.len = 0;
  a.kind = kind;
  a.resource = tracker ? tracker->resource : 0;
  return a;
}

