/*
 * fasync_dep.h -- declared effect sets and automatic dependency construction.
 *
 * This is idea.md section 3.2. The problem it solves: if two syscalls share no
 * value in the source but secretly touch the same memory, the compiler cannot
 * see the dependency from dataflow alone. The explicit-dataflow case (section
 * 3.1) is free -- the pending tag on a result *is* the dependency edge -- but
 * hidden aliasing needs the programmer to say something.
 *
 * The design follows OpenMP task depend clauses and, before them, Jade:
 * each operation declares the set of ranges it reads (in) and writes (out), and
 * the runtime derives the whole dependency DAG from those declarations. Nobody
 * writes a lock or a barrier.
 *
 * THE PART THAT IS SPECIFIC TO FIL-C
 * ----------------------------------
 * The declarations are a fallback, not the primary mechanism. Because every
 * Fil-C pointer carries a capability with real bounds, the analysis can ask the
 * runtime for the true extent of the object behind a pointer (zgetlower /
 * zgetupper) and prove two operands disjoint outright. When that proof succeeds,
 * no edge is needed even if the declared access kinds would have conflicted.
 *
 * That is the same relationship `restrict` has to C's alias analysis: the
 * declaration is an escape valve for the cases the analysis cannot resolve on
 * its own, and the better the analysis, the less often the programmer has to
 * reach for it.
 */

#ifndef FASYNC_DEP_H
#define FASYNC_DEP_H

#include <stddef.h>

#include "fasync.h"

/*
 * One declared access: either a byte range, or a *named resource*.
 *
 * `kind` uses the FASYNC_IN / FASYNC_OUT / FASYNC_INOUT constants from
 * fasync.h, matching OpenMP's depend(in:) / depend(out:) / depend(inout:).
 *
 * The named-resource form exists because not every dependency is a byte range.
 * Two calls can collide on a file descriptor, a path, a device or a lock --
 * things the capability-range analysis has nothing to say about. A resource is
 * identified by an integer, and the rule is: two accesses to the same resource
 * conflict when at least one writes, and accesses to different resources never
 * conflict.
 *
 * async-a-sync.pdf's serialization token is exactly a resource claimed INOUT by
 * everyone who shares it, which is why it needs no separate machinery below --
 * see fasync_tracker.
 */
struct fasync_access {
  const void* buf;         /* byte range, or NULL for a resource access   */
  size_t len;              /* length of that range                        */
  unsigned kind;           /* FASYNC_IN / OUT / INOUT                     */
  unsigned long resource;  /* named resource id; ignored when buf != NULL */
};

/* A byte range. */
#define FASYNC_ACCESS_RANGE(ptr, len, k) \
  ((struct fasync_access){(ptr), (len), (k), 0})

/* A named resource: an fd, a path, a lock, or a serialization token. */
#define FASYNC_ACCESS_RESOURCE(id, k) \
  ((struct fasync_access){0, 0, (k), (id)})

/*
 * One operation, with its declared effect set.
 *
 * A real program expresses this as a pragma immediately preceding the call:
 *
 *     #pragma filc_syscall depend(in: buf1, out: buf2)
 *     read(fd, buf1, buf2 ...);
 *
 * The runtime form is a struct because there is no pragma without the compiler
 * patch; see compiler/README.md for the source-level form.
 */
struct fasync_op {
  const char* name;                  /* reporting only */
  const struct fasync_access* accesses;
  unsigned n_accesses;
};

/*
 * What building a DAG cost. `auto_disjoint_pairs` is the interesting number: it
 * counts pairs whose declared access kinds conflicted but whose capability
 * ranges proved disjoint, so no edge was needed and the two operations run
 * concurrently. That is the annotation the programmer did not have to write.
 */
struct fasync_dag_stats {
  unsigned ops;
  unsigned edges;                /* edges actually emitted                 */
  unsigned declared_edges;       /* edges from a genuine declared conflict */
  unsigned auto_disjoint_pairs;  /* conflicts dissolved by the proof       */
  unsigned read_write_edges;     /* writer -> reader                       */
  unsigned write_read_edges;     /* reader -> writer (anti-dependency)     */
  unsigned write_write_edges;    /* writer -> writer                       */
};

/*
 * Do two operations conflict, and if their declarations say they might, was the
 * conflict dissolved by proving the capability ranges disjoint?
 *
 * Returns 1 if `b` must run after `a`. Sets *auto_disjoint to 1 when the
 * declarations overlapped but the ranges did not.
 */
int fasync_ops_conflict(const struct fasync_op* a, const struct fasync_op* b,
                        int* auto_disjoint);

/*
 * Build the dependency DAG over `ops`.
 *
 * An edge is emitted for every ordered pair (i, j) with i < j where j depends on
 * i. Edges are written into `edges` as `from * n_ops + to`, up to `max_edges`.
 * Returns the number of edges written, or 0 on error.
 */
unsigned fasync_build_dag(const struct fasync_op* ops, unsigned n_ops,
                          unsigned* edges, unsigned max_edges,
                          struct fasync_dag_stats* stats);

/* ------------------------------------------------------------------ */
/* Serialization tokens (async-a-sync.pdf)                             */
/* ------------------------------------------------------------------ */

/*
 * A token passed to every call that must be ordered against the others sharing
 * it. This is the mechanism proposed in async-a-sync.pdf, where it appears as a
 * hidden trailing argument:
 *
 *     // signature written by the programmer
 *     int balls(int* a, int b);
 *     // signature actually generated
 *     int balls(int* a, int b, void* PROVENANCE = 0);
 *
 *     void* PROVTRACKER = prov_alloc();
 *     balls(&a, b, PROVTRACKER);   // ordered against everything else on it
 *     balls(&c, d, PROVTRACKER);
 *
 * The token is modelled here as a named resource claimed INOUT by everyone who
 * shares it, so it flows through the ordinary dependency analysis instead of
 * needing its own path. That also means tokens and effect sets compose: a call
 * can share a token *and* declare precise buffer ranges, and the resulting DAG
 * carries both sets of edges.
 *
 * What a token cannot express is anything but a chain. Everyone sharing one token
 * is ordered against everyone else sharing it, including pairs that do not
 * actually conflict. docs/ARCHITECTURE.md has the measured cost of that.
 */
typedef struct fasync_tracker {
  unsigned long resource;
} fasync_tracker;

/* Allocate a token. Returns NULL if the resource id space is exhausted. */
fasync_tracker* fasync_tracker_new(void);

void fasync_tracker_free(fasync_tracker* tracker);

/*
 * The access to declare for an operation that shares this token. FASYNC_INOUT
 * reproduces async-a-sync.pdf's semantics; FASYNC_IN lets several operations
 * share a token as readers, which the PDF's mechanism cannot express.
 */
struct fasync_access fasync_tracker_access(const fasync_tracker* tracker,
                                           unsigned kind);

/* ------------------------------------------------------------------ */
/* Execution                                                           */
/* ------------------------------------------------------------------ */

/*
 * Called when an operation becomes ready (i.e. all its dependencies have
 * completed). It enqueues the work and returns a handle, or 0 on failure.
 */
typedef fasync_id (*fasync_submit_fn)(const struct fasync_op* op, void* ctx);

/*
 * How a DAG execution went.
 *
 * `max_concurrent` is the point of the exercise: it is the peak number of
 * operations in flight at once, i.e. the parallelism the declarations actually
 * unlocked. A coarse serialization pragma would drive this to 1.
 */
struct fasync_dag_run {
  unsigned submitted;
  unsigned max_concurrent;
  unsigned waves;
};

/*
 * Execute `ops` in an order consistent with `edges`, exposing every operation
 * that is ready at once.
 *
 * This is deliberately single-threaded: parallelism here comes from having
 * several syscalls in flight simultaneously, not from having several threads.
 * An operation is submitted as soon as its dependencies are satisfied, and the
 * scheduler moves on without waiting for it, which is exactly the property the
 * whole design is about.
 */
int fasync_run_dag(const struct fasync_op* ops, unsigned n_ops,
                   const unsigned* edges, unsigned n_edges,
                   fasync_submit_fn submit, void* ctx,
                   struct fasync_dag_run* out);

#endif /* FASYNC_DEP_H */
