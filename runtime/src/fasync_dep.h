/*
 * fasync_dep.h -- declared effect sets and automatic dependency construction
 * (idea.md section 3.2). Some ops share memory no dataflow can see; these
 * declarations are the escape valve, in the OpenMP task-depend / Jade style.
 * Fil-C's capability bounds let the analysis *prove* two declared accesses
 * disjoint outright, which is how the annotation burden stays low -- the same
 * relationship `restrict` has to C's alias analysis.
 */
#pragma once

#include <stddef.h>

#include "fasync.h"

/* One declared access: a byte range, or a *named resource* (`buf == 0`). Two
 * accesses to the same resource conflict when at least one writes; accesses to
 * different resources never conflict. async-a-sync.pdf's serialization token is
 * exactly a resource claimed INOUT by everyone who shares it. */
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

/* One operation, with its declared effect set. A real program would say this as
 * `#pragma filc_syscall depend(in: buf1, out: buf2)`; the struct form exists
 * because there is no pragma without the compiler patch. */
struct fasync_op {
  const char* name;                  /* reporting only */
  const struct fasync_access* accesses;
  unsigned n_accesses;
};

/* What building a DAG cost. `auto_disjoint_pairs` is the interesting number:
 * pairs whose kinds conflicted but whose capability ranges proved disjoint, so
 * no edge was needed -- the annotations the programmer did not have to write. */
struct fasync_dag_stats {
  unsigned ops;
  unsigned edges;                /* edges actually emitted                 */
  unsigned declared_edges;       /* edges from a genuine declared conflict */
  unsigned auto_disjoint_pairs;  /* conflicts dissolved by the proof       */
  unsigned read_write_edges;     /* writer -> reader                       */
  unsigned write_read_edges;     /* reader -> writer (anti-dependency)     */
  unsigned write_write_edges;    /* writer -> writer                       */
};

/* Do `a` and `b` conflict? 1 if `b` must run after `a`; *auto_disjoint is set
 * when the declarations overlapped but the ranges proved disjoint. */
int fasync_ops_conflict(const struct fasync_op* a, const struct fasync_op* b,
                        int* auto_disjoint);

/* Build the dependency DAG over `ops`: an edge for every ordered pair (i, j),
 * i < j, where j depends on i, written as `from * n_ops + to` up to
 * `max_edges`. Returns the number of edges, or 0 on error. */
unsigned fasync_build_dag(const struct fasync_op* ops, unsigned n_ops,
                          unsigned* edges, unsigned max_edges,
                          struct fasync_dag_stats* stats);

/* Serialization tokens (async-a-sync.pdf): a token passed to every call that
 * must be ordered against the others sharing it -- a named resource claimed
 * INOUT, so it flows through the ordinary dependency analysis. A token can only
 * express a chain: everyone sharing one is ordered against everyone else,
 * including pairs that do not actually conflict (docs/ARCHITECTURE.md measures
 * that cost). */
#define FASYNC_TOKEN_QLEN 16

typedef struct fasync_tracker {
  unsigned long resource; /* the named resource the DAG layer orders on    */
  unsigned n;             /* ops still in flight on this token            */
  struct {
    fasync_id id;         /* handle of an issued, not-yet-complete op      */
    unsigned kind;        /* the access kind it was issued with            */
  } q[FASYNC_TOKEN_QLEN];
} fasync_tracker;

/* Allocate a token, or NULL if the resource id space is exhausted. */
fasync_tracker* fasync_tracker_new(void);

void fasync_tracker_free(fasync_tracker* tracker);

/* The access to declare for an operation sharing this token: FASYNC_INOUT
 * reproduces the PDF's semantics; FASYNC_IN lets several ops share a token as
 * readers, which the PDF's mechanism cannot express. */
struct fasync_access fasync_tracker_access(const fasync_tracker* tracker,
                                           unsigned kind);

/* Direct runtime ordering on a token (the PDF's hidden trailing argument, made
 * explicit): fasync_pread/pwrite with the token folded in. A tagged call is not
 * enqueued until every *conflicting* tagged call already issued on the token --
 * at least one side writes, i.e. the rule fasync_ops_conflict applies to its
 * declared accesses; reads overlap freely -- has finished. The returned handle
 * is an ordinary async handle afterwards. */
fasync_id fasync_tagged_pread(int fd, void* buf, size_t len, unsigned long offset,
                              fasync_tracker* tok, unsigned kind);
fasync_id fasync_tagged_pwrite(int fd, void* buf, size_t len, unsigned long offset,
                               fasync_tracker* tok, unsigned kind);

/* Execution. */
/* Called when an operation becomes ready (all dependencies completed); it
 * enqueues the work and returns a handle, or 0 on failure. */
typedef fasync_id (*fasync_submit_fn)(const struct fasync_op* op, void* ctx);

/* How a DAG execution went. `max_concurrent` is the point: peak operations in
 * flight, i.e. the parallelism the declarations actually unlocked. */
struct fasync_dag_run {
  unsigned submitted;
  unsigned max_concurrent;
  unsigned waves;
};

/* Execute `ops` in an order consistent with `edges`, submitting every operation
 * that is ready at once. Deliberately single-threaded: parallelism comes from
 * several syscalls in flight, not from several threads. */
int fasync_run_dag(const struct fasync_op* ops, unsigned n_ops,
                   const unsigned* edges, unsigned n_edges,
                   fasync_submit_fn submit, void* ctx,
                   struct fasync_dag_run* out);