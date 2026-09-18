/* fasync_syscalls.h -- the io_uring syscalls and the bridge. */
#pragma once

#include <stddef.h>

long zsys_io_uring_setup(unsigned entries, void* params);

long zsys_io_uring_enter(int ring_fd, unsigned to_submit, unsigned min_complete,
                         unsigned flags);

long zsys_io_uring_register(int ring_fd, unsigned opcode, void* arg,
                            unsigned long nr_args);

/* Bridge calls into the native half. */
void fasync_publish_state(void* shared_state); /* once at ring setup */
void fasync_poll(void);                        /* reap without blocking */
void fasync_block(void);                       /* sleep until a completion */