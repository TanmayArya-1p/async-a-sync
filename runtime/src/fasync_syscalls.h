/*
 * fasync_syscalls.h -- the io_uring syscall surface, as exposed by libpizlo.
 *
 * These functions are added to the trusted runtime by this project; see
 * runtime/patches/0001-libpas-io_uring-forwarders.patch and fasync_native.c.
 *
 * They are pizlonated functions. When filcc compiles a call to
 * zsys_io_uring_enter(), the reference it emits is to the symbol
 * pizlonated_zsys_io_uring_enter, which the regenerated forwarder table in
 * libpizlo provides; that wrapper marshals the arguments into the native
 * implementation, which is where the syscall instruction actually runs.
 *
 * The declarations live here rather than being added to libpizlo's own
 * pizlonated_syscalls.h so that the extension stays purely additive and does not
 * modify shipped headers.
 *
 * All three follow the ordinary syscall convention: >= 0 on success, -1 with
 * errno set on failure.
 */

#ifndef FASYNC_SYSCALLS_H
#define FASYNC_SYSCALLS_H

#include <stddef.h>

/* io_uring_setup: returns the ring fd, or -1 with errno set. */
long zsys_io_uring_setup(unsigned entries, void* params);

/*
 * io_uring_enter: returns the number of completions reaped (or SQEs consumed).
 *
 * min_complete == 0 submits without waiting; min_complete > 0 may sleep in the
 * kernel until that many completions are available.
 */
long zsys_io_uring_enter(int ring_fd, unsigned to_submit, unsigned min_complete,
                         unsigned flags);

/* io_uring_register: returns 0, or -1 with errno set. */
long zsys_io_uring_register(int ring_fd, unsigned opcode, void* arg,
                            unsigned long nr_args);

/* ------------------------------------------------------------------ */
/* Async runtime entry points                                          */
/*                                                                     */
/* These are implemented natively in fasync_native.c and reached through */
/* the generated forwarder table, the same way the io_uring syscalls     */
/* above are. The memory-safe half calls them; the compiler-inserted hook */
/* calls the same native policy directly, under the name               */
/* filc_resolve_pending (see fasync_shared.h).                          */
/* ------------------------------------------------------------------ */

/* Hand the native resolver the addresses it needs. Called once, at ring
 * setup; the addresses are retained, not copied. */
void fasync_publish_state(void* shared_state);

/* Reap whatever completions have landed, without blocking. */
void fasync_poll(void);

/* Sleep until at least one completion is available. */
void fasync_block(void);

#endif /* FASYNC_SYSCALLS_H */
