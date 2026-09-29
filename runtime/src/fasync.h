#pragma once

#include <stddef.h>

typedef unsigned long fasync_id; /* one in-flight request */

/* Threading: the runtime is single-threaded. One io_uring ring and request
 * table serve the process, without locks. The thread that makes the first
 * request owns them; a request, a wait, or an access to a buffer with a
 * request in flight from any other thread stops the program. Other threads
 * may compute on memory of their own.
 *
 * A buffer passed to fasync_pread is pending until its read completes: the
 * compiler's access hook, through the async framework, waits for it on first
 * access. Code the patched compiler did not build calls fasync_resolve_pending
 * (or FASYNC_ACCESS) before touching such a buffer. */

void* fasync_resolve_pending(void* ptr, size_t size);

/* no-op with the patched compiler */
#ifdef FASYNC_COMPILER_INSERTS_CHECKS
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
/* no fasync_read because an offset is mandatory */
fasync_id fasync_openat(int dirfd, const char* path, int flags, int mode);

/* pending descriptor usable where an fd accepted */
int fasync_open_pending(int dirfd, const char* path, int flags, int mode);

long fasync_fd_resolve(int fd); /* real fd or waited pending handle */

int fasync_submit(void); /* publish all queued sqes */
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
  unsigned long parks;
  unsigned long completions_reaped;
};
void fasync_get_stats(struct fasync_stats* out);
void fasync_reset_stats(void);

const char* fasync_last_error(void);
