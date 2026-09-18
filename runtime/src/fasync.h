/* fasync.h -- the public surface of the async runtime. */
#pragma once

#include <stddef.h>

typedef unsigned long fasync_id; /* one in-flight request */

void* fasync_resolve_pending(void* ptr, size_t size);

/* The hook the patched compiler emits. */
void* filc_resolve_pending(void* ptr, size_t size);

/* Explicit resolution points that are a no-op with the patched compiler. */
#ifdef FASYNC_COMPILER_INSERTS_CHECKS /* patched compiler */
#define FASYNC_ACCESS(ptr, size) ((void)0)
#else
#define FASYNC_ACCESS(ptr, size) ((void)fasync_resolve_pending((void*)(ptr), (size)))
#endif

enum fasync_access_kind {
  FASYNC_IN = 0,   /* reads this range */
  FASYNC_OUT = 1,  /* writes this range */
  FASYNC_INOUT = 2 /* may do both */
};

typedef unsigned int fasync_uint32; /* fixed-size ABI type */

fasync_id fasync_pread(int fd, void* buf, size_t len, unsigned long offset);
fasync_id fasync_pwrite(int fd, void* buf, size_t len, unsigned long offset);
fasync_id fasync_fsync(int fd);
fasync_id fasync_close(int fd);
fasync_id fasync_openat(int dirfd, const char* path, int flags, int mode);
/* No fasync_read() because an offset is mandatory. */

/* Pending descriptor usable anywhere an fd is accepted. */
int fasync_open_pending(int dirfd, const char* path, int flags, int mode);

long fasync_fd_resolve(int fd); /* real fd or a waited pending handle */
/* fasync_openat_direct arrives with promise-pipelining. */

int fasync_submit(void); /* publish all queued SQEs */
int fasync_ready(fasync_id id);
int fasync_wait_all(void);
long fasync_result(fasync_id id); /* bytes transferred or -errno */

struct fasync_prov {
  fasync_id req;
  unsigned long offset;
  unsigned long len;
};
int fasync_provenance(const void* ptr, size_t size, struct fasync_prov* out);
void* fasync_derive(void* base, unsigned long offset, size_t len,
                    struct fasync_prov* out);

struct fasync_stats {
  unsigned long sqes_queued;
  unsigned long kernel_submit_entries;
  unsigned long kernel_wait_entries; /* context switches actually taken */
  unsigned long userspace_cq_polls;
  unsigned long resolve_calls;
  unsigned long fast_path_hits;
  unsigned long spin_rounds;
  unsigned long parks;
  unsigned long completions_reaped;
  unsigned long memo_hits;
};
void fasync_get_stats(struct fasync_stats* out);
void fasync_reset_stats(void);

const char* fasync_last_error(void);