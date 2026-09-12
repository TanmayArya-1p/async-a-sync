/*
 * fasync_native.c -- the native half of the async syscall runtime.
 *
 * This file is compiled by the HOST compiler, not by filcc, and includes
 * libpas's internal headers. That is what makes it part of Fil-C's *trusted
 * runtime core* -- the small body of code that is allowed to be unsafe, and
 * outside of which Fil-C guarantees no unsafe code can be linked.
 *
 * WHY IO_URING HAS TO LIVE HERE
 * -----------------------------
 * Fil-C has no escape hatch, and it enforces that. Trying to reach io_uring from
 * memory-safe code fails at compile time:
 *
 *     filc safety error: cannot handle inline asm (unsupported mnemonic for
 *     safe inline asm: syscall)
 *     filc panic: thwarted a futile attempt to violate memory safety.
 *
 * And routing around it via syscall(2) does not help either: zsys_syscall's
 * dispatcher has an allowlist and rejects io_uring outright:
 *
 *     filc user error: unsupported syscall: 425.
 *         (libpizlo.so) ../filc/src/runtime.c:656: zsys_syscall
 *
 * So io_uring is added the only way it can be: as first-class syscalls in the
 * trusted runtime, alongside zsys_read and zsys_openat. The signatures are
 * declared in libpas/src/libpas/generate_pizlonated_forwarders.rb, which
 * generates the pizlonated wrapper that memory-safe code actually calls. That
 * generator is the sanctioned extension point for this layer, and it is where
 * the whole design's "hook in libpizlo where syscalls are already recognized as
 * a checked boundary" (idea.md section 2.6) lands in practice.
 *
 * THE IDIOM
 * ---------
 * These follow the exact shape of the existing pizlonated syscalls:
 *
 *     ssize_t filc_native_zsys_read(filc_thread* my_thread, int fd,
 *                                   filc_ptr buf, size_t size)
 *     {
 *         check_fd(fd);
 *         filc_check_write(buf, size);
 *         return FILC_SYSCALL(my_thread, read(fd, filc_ptr_ptr(buf), size));
 *     }
 *
 * i.e. validate every pointer argument against the capability model, then
 * bracket the syscall in the GC safepoint protocol. The only difference is that
 * io_uring has no libc wrapper, so the syscall instruction is issued directly
 * and errno is reconstructed from the raw return value.
 */

#include "filc_runtime.h"

#include "fasync_io_uring.h"

/* ------------------------------------------------------------------ */
/* Raw syscall primitives                                              */
/*                                                                     */
/* Only legal here. The same definitions in a filcc-compiled file are   */
/* refused by the compiler.                                            */
/* ------------------------------------------------------------------ */

static PAS_ALWAYS_INLINE long fasync_syscall2(long n, long a, long b) {
  long ret;
  __asm__ volatile("syscall"
                   : "=a"(ret)
                   : "a"(n), "D"(a), "S"(b)
                   : "rcx", "r11", "memory");
  return ret;
}

static PAS_ALWAYS_INLINE long fasync_syscall4(long n, long a, long b, long c,
                                              long d) {
  long ret;
  register long r10 __asm__("r10") = d;
  __asm__ volatile("syscall"
                   : "=a"(ret)
                   : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10)
                   : "rcx", "r11", "memory");
  return ret;
}

static PAS_ALWAYS_INLINE long fasync_syscall6(long n, long a, long b, long c,
                                              long d, long e, long f) {
  long ret;
  register long r10 __asm__("r10") = d;
  register long r8 __asm__("r8") = e;
  register long r9 __asm__("r9") = f;
  __asm__ volatile("syscall"
                   : "=a"(ret)
                   : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8),
                     "r"(r9)
                   : "rcx", "r11", "memory");
  return ret;
}

/*
 * Raw syscalls return -errno rather than setting errno. This converts to the
 * convention the rest of the pizlonated syscall surface uses -- return -1 and
 * set errno -- so that memory-safe callers can be written as ordinary C.
 */
static PAS_ALWAYS_INLINE long fasync_finish(long ret) {
  if (ret < 0 && ret >= -4095) {
    filc_set_errno((int)-ret);
    return -1;
  }
  return ret;
}

/* ------------------------------------------------------------------ */
/* io_uring_setup                                                      */
/* ------------------------------------------------------------------ */

/*
 * io_uring_setup(entries, params) -> ring fd, or -1 with errno set.
 *
 * `params` is written by the kernel, so it takes filc_check_write. Note that it
 * is the runtime's own stack object (see fasync_ring_init in fasync.c), never a
 * program-supplied pointer, but the check is performed anyway: the point of a
 * checked boundary is that it does not rely on the caller's good behaviour.
 *
 * No safepoint bracket: this call never blocks.
 */
PAS_API long filc_native_zsys_io_uring_setup(filc_thread* my_thread,
                                             unsigned entries,
                                             filc_ptr params) {
  PAS_UNUSED_PARAM(my_thread);
  filc_check_write(params, sizeof(struct fasync_params));
  return fasync_finish(
      fasync_syscall2(FASYNC_SYS_io_uring_setup, (long)entries,
                      (long)filc_ptr_ptr(params)));
}

/* ------------------------------------------------------------------ */
/* io_uring_enter                                                      */
/* ------------------------------------------------------------------ */

/*
 * io_uring_enter(fd, to_submit, min_complete, flags) -> submitted count, or -1.
 *
 * The kernel's real signature has two trailing signal-mask arguments, both
 * always NULL here: interrupt-driven reaping is out of scope for this
 * prototype, and a NULL sigset means "do not block signals", which is what we
 * want.
 *
 * THE SAFEPOINT SPLIT IS THE IMPORTANT PART.
 *
 *   min_complete == 0  -- "publish these SQEs". Returns promptly. No safepoint,
 *                         because this is the submission fast path and paying a
 *                         stop-the-world handshake here would defeat the entire
 *                         point of zero-context-switch submission.
 *
 *   min_complete  > 0  -- "sleep until a completion is available". This can
 *                         block indefinitely, so the thread must leave the
 *                         Fil-C world first: otherwise a collector handshake
 *                         could wait forever on a thread parked in the kernel,
 *                         and the collector would be entitled to assume it can
 *                         scan a stack that is actually frozen mid-syscall.
 */
PAS_API long filc_native_zsys_io_uring_enter(filc_thread* my_thread,
                                             int ring_fd, unsigned to_submit,
                                             unsigned min_complete,
                                             unsigned flags) {
  if (!min_complete)
    return fasync_finish(fasync_syscall6(FASYNC_SYS_io_uring_enter,
                                         (long)ring_fd, (long)to_submit, 0L,
                                         (long)flags, 0L, 0L));

  filc_exit(my_thread);
  long ret = fasync_syscall6(FASYNC_SYS_io_uring_enter, (long)ring_fd,
                             (long)to_submit, (long)min_complete, (long)flags,
                             0L, 0L);
  filc_enter(my_thread);

  return fasync_finish(ret);
}

/* ------------------------------------------------------------------ */
/* io_uring_register                                                   */
/* ------------------------------------------------------------------ */

/*
 * io_uring_register(fd, opcode, arg, nr_args) -> 0, or -1 with errno set.
 *
 * `arg` is interpreted according to `opcode` (a buffer array, a file table, a
 * set of eventfds, ...). It is read by the kernel and written back on some
 * opcodes, so it is checked as writable.
 *
 * Bracketed by a safepoint because registering buffers and file tables can
 * touch every page involved and is not bounded in the way a queue submission is.
 */
/* ------------------------------------------------------------------ */
