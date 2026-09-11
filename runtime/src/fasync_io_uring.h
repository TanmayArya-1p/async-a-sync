/*
 * fasync_io_uring.h -- the io_uring kernel ABI, in one place.
 *
 * Included by both halves of the runtime:
 *
 *   fasync.c         compiled by filcc  (memory-safe, capability-checked)
 *   fasync_native.c  compiled by clang (native, issues the raw syscalls)
 *
 * They must agree on these layouts byte for byte, and the kernel must agree with
 * both, so the definitions live here rather than being duplicated. They are
 * written out by hand instead of including <linux/io_uring.h> so that the exact
 * layout in use is auditable and so the runtime does not depend on kernel
 * headers being installed.
 *
 * Reference: include/uapi/linux/io_uring.h
 */

#ifndef FASYNC_IO_URING_H
#define FASYNC_IO_URING_H

#include <stddef.h>

/* ------------------------------------------------------------------ */
/* Syscall numbers (x86_64)                                            */
/* ------------------------------------------------------------------ */
#define FASYNC_SYS_io_uring_setup 425
#define FASYNC_SYS_io_uring_enter 426
#define FASYNC_SYS_io_uring_register 427

/* ------------------------------------------------------------------ */
/* Submission queue entry                                              */
/* ------------------------------------------------------------------ */
struct fasync_sqe {
  unsigned char opcode;
  unsigned char flags;
  unsigned short ioprio;
  int fd;
  union {
    unsigned long off;
    unsigned long addr2;
  };
  union {
    unsigned long addr;
    unsigned long splice_off_in;
  };
  unsigned int len;
  union {
    unsigned int rw_flags;
    unsigned int fsync_flags;
    unsigned int open_flags;
    unsigned int statx_flags;
    unsigned int msg_flags;
    unsigned int timeout_flags;
    unsigned int accept_flags;
    unsigned int cancel_flags;
  };
  unsigned long user_data;
  union {
    unsigned short buf_index;
    unsigned short buf_group;
  };
  unsigned short personality;
  union {
    int splice_fd_in;
    unsigned int file_index;
    struct {
      unsigned short addr_len;
      unsigned short pad3[1];
    };
  };
  union {
    struct {
      unsigned long addr3;
      unsigned long pad2[1];
    };
    unsigned char cmd[0];
  };
};

struct fasync_cqe {
  unsigned long user_data;
  int res;
  unsigned int flags;
};

struct fasync_sqring_offsets {
  unsigned int head;
  unsigned int tail;
  unsigned int ring_mask;
  unsigned int ring_entries;
  unsigned int flags;
  unsigned int dropped;
  unsigned int array;
  unsigned int resv1;
  unsigned long user_addr;
};

struct fasync_cqring_offsets {
  unsigned int head;
  unsigned int tail;
  unsigned int ring_mask;
  unsigned int ring_entries;
  unsigned int overflow;
  unsigned int cqes;
  unsigned int flags;
  unsigned int resv1;
  unsigned long user_addr;
};

struct fasync_params {
  unsigned int sq_entries;
  unsigned int cq_entries;
  unsigned int flags;
  unsigned int sq_thread_cpu;
  unsigned int sq_thread_idle;
  unsigned int features;
  unsigned int wq_fd;
  unsigned int resv[3];
  struct fasync_sqring_offsets sq_off;
  struct fasync_cqring_offsets cq_off;
};

/* ------------------------------------------------------------------ */
/* mmap offsets for the ring regions                                   */
/* ------------------------------------------------------------------ */
#define FASYNC_OFF_SQ_RING 0UL
#define FASYNC_OFF_CQ_RING 0x8000000UL
#define FASYNC_OFF_SQES 0x10000000UL

/* io_uring_params.flags */
#define FASYNC_SETUP_NO_MMAP (1U << 14U)

/*
 * Room reserved for the SQ+CQ rings when the caller supplies the memory
 * (FASYNC_SETUP_NO_MMAP). The exact size is only known after setup, so this is
 * a generous upper bound; the kernel uses only what it needs.
 */
#define FASYNC_RINGS_BYTES (2U * 1024U * 1024U)

/* io_uring_params.features */
#define FASYNC_FEAT_SINGLE_MMAP (1U << 0U)

/* io_uring opcodes we use */
#define FASYNC_OP_NOP 0
#define FASYNC_OP_FSYNC 3
#define FASYNC_OP_OPENAT 18
#define FASYNC_OP_CLOSE 19
#define FASYNC_OP_READ 22
#define FASYNC_OP_WRITE 23

/* SQE flags. IO_LINK makes this SQE wait for the previous one to complete,
 * which is what builds kernel-native promise chains. */
#define FASYNC_SQE_IO_LINK (1U << 2U)

/* io_uring_register opcodes */
#define FASYNC_REGISTER_BUFFERS 0
#define FASYNC_REGISTER_FILES 2

#endif /* FASYNC_IO_URING_H */
