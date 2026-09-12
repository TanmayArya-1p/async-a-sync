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
/* The compiler-facing resolution hook                                 */
/* ------------------------------------------------------------------ */

/*
 * This is where the patched FilPizlonator's emitted call lands, and it has to be
 * native. See fasync_shared.h for why: a `pizlonated_*` entry point is an 8-byte
 * descriptor stub, so a direct call to one returns a function object rather than
 * running the body, and reaching the body requires the closure protocol. Every
 * other function the pass emits calls to -- filc_check_function_call_fail and
 * friends -- is native for the same reason.
 *
 * Nothing here owns state: `fasync_published` points at the memory-safe half's
 * request table and completion ring, published once at ring setup. What lives
 * here is the resolution *policy*, because this is the only place the compiler
 * can reach.
 */
#include "fasync_shared.h"

static struct fasync_shared* volatile fasync_published;

/* How long to poll the completion ring before agreeing to sleep in the kernel.
 * Polling is a plain read of shared memory and therefore cheap; parking is the
 * expensive operation the design exists to avoid. Must match the budget in
 * fasync.c. */
#define FASYNC_NATIVE_SPIN_LIMIT 20000

/*
 * Publish the shared state. Called once, from the memory-safe half at ring setup,
 * through the generator-bridged wrapper (the fasync_publish_state addSig entry).
 *
 * The address is retained, not copied, which is why the memory it points into has
 * to be stable: the ring is GC memory and the request table is a static array.
 * tests/stage2c_gc_pin_probe.c is the check that the GC does not relocate the
 * former.
 */
PAS_API void filc_native_fasync_publish_state(filc_thread* my_thread,
                                              filc_ptr state) {
  PAS_UNUSED_PARAM(my_thread);
  fasync_published = (struct fasync_shared*)filc_ptr_ptr(state);
}

/*
 * Reap whatever completions have landed. A pure userspace read of the shared
 * completion ring: no syscall, no mode switch, no kernel entry.
 */
static void fasync_native_drain(struct fasync_shared* sh) {
  if (!sh || !sh->cqes)
    return;

  (*sh->userspace_cq_polls)++;

  unsigned int mask = *sh->cq_mask;
  unsigned int head = *sh->local_cq_head;
  unsigned int tail = __atomic_load_n(sh->cq_tail, __ATOMIC_ACQUIRE);
  unsigned int count = 0;

  while (head != tail) {
    struct fasync_cqe cqe = sh->cqes[head & mask];
    head++;

    unsigned int index = (unsigned int)(cqe.user_data & 0xFFFFFFFFUL);
    if (index < sh->n_reqs) {
      struct fasync_req_shared* r = &sh->reqs[index];
      if (r->state == FASYNC_REQ_PENDING && r->id == cqe.user_data) {
        r->result = cqe.res;
        r->state = cqe.res < 0 ? FASYNC_REQ_FAILED : FASYNC_REQ_DONE;
        /* Retire only after the final state is visible, so a racing resolver
         * either sees the request pending (and looks again) or sees it
         * retired. */
        __atomic_sub_fetch(sh->inflight, 1, __ATOMIC_RELEASE);
      }
    }
    count++;
  }

  *sh->local_cq_head = head;
  __atomic_store_n(sh->cq_head, head, __ATOMIC_RELEASE);
  *sh->completions_reaped += count;
}

/*
 * The pending request covering [ptr, ptr+size), or NULL.
 *
 * A completed request is excluded, which is the self-healing step: once a request
 * resolves, its range stops being reported as pending, so later accesses fall
 * straight through to the fast path.
 */
static struct fasync_req_shared* fasync_native_find(struct fasync_shared* sh,
                                                    const void* ptr,
                                                    size_t size) {
  const char* p = (const char*)ptr;
  for (unsigned long i = 0; i < sh->n_reqs; i++) {
    struct fasync_req_shared* r = &sh->reqs[i];
    if (r->state != FASYNC_REQ_PENDING || !r->buf)
      continue;
    const char* start = (const char*)r->buf;
    const char* end = start + r->len;
    if (p < start)
      continue;
    if (p + size > end)
      continue;
    return r;
  }
  return 0;
}

/*
 * filc_resolve_pending -- what the compiler emits calls to.
 *
 * THIS IS THE HOT PATH: it runs on every instrumented access, so its cost when
 * nothing is pending is the design's most important number. One acquire load of a
 * cache-resident counter and a predictable branch.
 *
 * Spins against the completion ring first and parks only second, because the
 * common case is that the completion has already landed and finding that out
 * should cost a load rather than a context switch.
 */
PAS_API void* filc_resolve_pending(void* ptr, size_t size) {
  if (!ptr)
    return ptr;

  struct fasync_shared* sh = fasync_published;
  if (!sh)
    return ptr;

  if (__atomic_load_n(sh->inflight, __ATOMIC_ACQUIRE) == 0) {
    (*sh->fast_path_hits)++;
    return ptr;
  }

  (*sh->resolve_calls)++;

  struct fasync_req_shared* r = fasync_native_find(sh, ptr, size);
  if (!r)
    return ptr;

  for (unsigned int spin = 0; spin < FASYNC_NATIVE_SPIN_LIMIT; spin++) {
    if (r->state != FASYNC_REQ_PENDING)
      return ptr;
    (*sh->spin_rounds)++;
    fasync_native_drain(sh);
    if (r->state != FASYNC_REQ_PENDING)
      return ptr;
#ifdef __x86_64__
    __builtin_ia32_pause();
#endif
  }

  /*
   * A full spin budget without a completion means there is nothing to be gained
   * by spinning further, so sleep until the kernel has something and look again.
   * This is the one call here that can block indefinitely, so it is the one that
   * takes the GC safepoint: otherwise a collector handshake could wait forever on
   * a thread parked in the kernel.
   */
  while (r->state == FASYNC_REQ_PENDING) {
    (*sh->parks)++;
    (*sh->kernel_wait_entries)++;
    filc_thread* my_thread = filc_get_my_thread();
    if (my_thread)
      filc_exit(my_thread);
    fasync_syscall6(FASYNC_SYS_io_uring_enter, (long)sh->ring_fd, 0L, 1L, 0L, 0L,
                    0L);
    if (my_thread)
      filc_enter(my_thread);
    fasync_native_drain(sh);
  }

  return ptr;
}

/* Non-blocking: reap whatever has landed, so a readiness check is a userspace
 * ring read rather than a syscall. */
PAS_API void filc_native_fasync_poll(filc_thread* my_thread) {
  PAS_UNUSED_PARAM(my_thread);
  fasync_native_drain(fasync_published);
}

/*
 * Block until at least one completion is available, for operations that produce
 * a scalar (an fd, an error code) rather than filling a buffer and therefore have
 * no range for the resolver to look up. Takes the GC safepoint, since it can
 * sleep indefinitely.
 */
PAS_API void filc_native_fasync_block(filc_thread* my_thread) {
  struct fasync_shared* sh = fasync_published;
  if (!sh)
    return;
  (*sh->parks)++;
  (*sh->kernel_wait_entries)++;
  if (my_thread)
    filc_exit(my_thread);
  fasync_syscall6(FASYNC_SYS_io_uring_enter, (long)sh->ring_fd, 0L, 1L, 0L, 0L,
                  0L);
  if (my_thread)
    filc_enter(my_thread);
  fasync_native_drain(sh);
}

PAS_API long filc_native_zsys_io_uring_register(filc_thread* my_thread,
                                                int ring_fd, unsigned opcode,
                                                filc_ptr arg,
                                                size_t nr_args) {
  filc_exit(my_thread);
  long ret = fasync_syscall4(FASYNC_SYS_io_uring_register, (long)ring_fd,
                             (long)opcode, (long)filc_ptr_ptr(arg),
                             (long)nr_args);
  filc_enter(my_thread);
  return fasync_finish(ret);
}
