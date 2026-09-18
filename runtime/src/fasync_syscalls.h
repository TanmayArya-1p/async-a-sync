/*
 * fasync_syscalls.h -- the io_uring syscall surface, as exposed by libpizlo.
 *
 * Added by runtime/patches/0001-libpas-io_uring-forwarders.patch (see
 * docs/RUNTIME.md): the generator emits pizlonated_* wrappers that marshal
 * into the native implementations in fasync_native.c. Declared here rather
 * than in pizlonated_syscalls.h so the extension stays purely additive.
 * Convention: >= 0 on success, -1 with errno set on failure.
 */
#pragma once

#include <stddef.h>

/* io_uring_setup(entries, params): returns the ring fd, or -1 with errno. */
long zsys_io_uring_setup(unsigned entries, void* params);

/* io_uring_enter(fd, to_submit, min_complete, flags): min_complete == 0
 * submits without waiting; > 0 may sleep until that many completions land. */
long zsys_io_uring_enter(int ring_fd, unsigned to_submit, unsigned min_complete,
                         unsigned flags);

/* io_uring_register(fd, opcode, arg, nr_args): 0, or -1 with errno. */
long zsys_io_uring_register(int ring_fd, unsigned opcode, void* arg,
                            unsigned long nr_args);

/* Async runtime entry points, implemented natively in fasync_native.c and
 * reached through the same forwarder table. The compiler-inserted hook calls
 * the same native policy directly under the name filc_resolve_pending. */

/* Hand the native resolver the addresses it needs. Once, at ring setup; they
 * are retained, not copied. */
void fasync_publish_state(void* shared_state);

/* Reap whatever completions have landed, without blocking. */
void fasync_poll(void);

/* Sleep until at least one completion is available. */
void fasync_block(void);