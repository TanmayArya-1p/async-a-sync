/* fasync_dep.h -- declared effect sets and automatic dependencies. */
#pragma once

#include <stddef.h>

#include "fasync.h"

struct fasync_access {
  const void* buf;         /* byte range or NULL for a resource */
  size_t len;
  unsigned kind;           /* FASYNC_IN OUT or INOUT */
  unsigned long resource;  /* ignored when buf is non NULL */
};

#define FASYNC_ACCESS_RANGE(ptr, len, k) \
  ((struct fasync_access){(ptr), (len), (k), 0})

#define FASYNC_ACCESS_RESOURCE(id, k) \
  ((struct fasync_access){0, 0, (k), (id)})

/* One operation with its declared effect set. */
struct fasync_op {
  const char* name; /* reporting only */
  const struct fasync_access* accesses;
  unsigned n_accesses;
};

struct fasync_dag_stats {
  unsigned ops;
  unsigned edges;
  unsigned declared_edges;
  unsigned auto_disjoint_pairs; /* conflicts dissolved by the range proof */
  unsigned read_write_edges;
  unsigned write_read_edges;
  unsigned write_write_edges;
};

int fasync_ops_conflict(const struct fasync_op* a, const struct fasync_op* b,
                        int* auto_disjoint);

unsigned fasync_build_dag(const struct fasync_op* ops, unsigned n_ops,
                          unsigned* edges, unsigned max_edges,
                          struct fasync_dag_stats* stats);

#define FASYNC_TOKEN_QLEN 16

typedef struct fasync_tracker {
  unsigned long resource; /* the named resource the DAG layer orders on */
  unsigned n;             /* ops still in flight on this token */
  struct {
    fasync_id id;         /* handle of an issued not-yet-complete op */
    unsigned kind;        /* the access kind it was issued with */
  } q[FASYNC_TOKEN_QLEN];
} fasync_tracker;

fasync_tracker* fasync_tracker_new(void);

void fasync_tracker_free(fasync_tracker* tracker);

struct fasync_access fasync_tracker_access(const fasync_tracker* tracker,
                                           unsigned kind);

/* Tagged calls wait for every conflicting tagged call on the token. */
fasync_id fasync_tagged_pread(int fd, void* buf, size_t len, unsigned long offset,
                              fasync_tracker* tok, unsigned kind);
fasync_id fasync_tagged_pwrite(int fd, void* buf, size_t len, unsigned long offset,
                               fasync_tracker* tok, unsigned kind);

typedef fasync_id (*fasync_submit_fn)(const struct fasync_op* op, void* ctx);

struct fasync_dag_run {
  unsigned submitted;
  unsigned max_concurrent; /* peak ops in flight */
  unsigned waves;
};

int fasync_run_dag(const struct fasync_op* ops, unsigned n_ops,
                   const unsigned* edges, unsigned n_edges,
                   fasync_submit_fn submit, void* ctx,
                   struct fasync_dag_run* out);