/*
 * fasync_native.c -- the native half of the async syscall runtime.
 *
 * Compiled by the HOST compiler (not filcc) and linked into libpizlo's trusted
 * runtime core: the small body of code allowed to be unsafe. This is why
 * io_uring has to live here -- Fil-C has no escape hatch, and inline asm in a
 * filcc file fails at compile time ("thwarted a futile attempt to violate
 * memory safety"), while zsys_syscall's allowlist rejects io_uring outright
 * ("unsupported syscall: 425"). So the three syscalls are added the only way
 * they can be: as first-class pizlonated syscalls, declared in
 * generate_pizlonated_forwarders.rb and generated into the forwarder table.
 *
 * Shape follows the existing pizlonated syscalls: validate every pointer
 * argument against the capability model, then bracket the syscall in the GC
 * safepoint protocol. The only difference is io_uring has no libc wrapper, so
 * the syscall instruction is issued directly and errno is reconstructed from
 * the raw return value.
 */

#include "filc_runtime.h"

#include "fasync_io_uring.h"

/* Raw syscall primitives. Only legal here -- the same inline asm in a
 * filcc-compiled file is refused by the compiler. */
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

/* Raw syscalls return -errno; convert to the -1-and-errno convention the rest
 * of the pizlonated surface uses so memory-safe callers stay plain C. */
static PAS_ALWAYS_INLINE long fasync_finish(long ret) {
  if (ret < 0 && ret >= -4095) {
    filc_set_errno((int)-ret);
    return -1;
  }
  return ret;
}

/* io_uring_setup(entries, params) -> ring fd, or -1 with errno set.
 * `params` is written by the kernel and is the runtime's own stack object, but
 * it is still checked writable: a checked boundary does not rely on the
 * caller's good behaviour. No safepoint: this call never blocks. */
PAS_API long filc_native_zsys_io_uring_setup(filc_thread* my_thread,
                                             unsigned entries,
                                             filc_ptr params) {
  PAS_UNUSED_PARAM(my_thread);
  filc_check_write(params, sizeof(struct fasync_params));
  return fasync_finish(
      fasync_syscall2(FASYNC_SYS_io_uring_setup, (long)entries,
                      (long)filc_ptr_ptr(params)));
}

/* io_uring_enter(fd, to_submit, min_complete, flags) -> submitted count, or -1.
 * The kernel's real signature has two trailing signal-mask arguments, both
 * always NULL here (no interrupt-driven reaping; NULL = do not block signals).
 *
 * THE SAFEPOINT SPLIT IS THE IMPORTANT PART.
 *   min_complete == 0: publish only, returns promptly. No safepoint, because a
 *      stop-the-world handshake here would defeat the whole point of
 *      zero-context-switch submission.
 *   min_complete > 0: may block indefinitely, so the thread must leave the
 *      Fil-C world first, or a collector handshake could wait forever on a
 *      thread parked in the kernel and entitled to assume its stack is mutable
 *      while it is actually frozen mid-syscall. */
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

/* io_uring_register(fd, opcode, arg, nr_args) -> 0, or -1 with errno set.
 * `arg` is interpreted by opcode (buffer array, file table, eventfds...) and is
 * checked writable. Bracketed by a safepoint: registering buffers or files can
 * touch every page involved and is not bounded like a queue submission. */
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

/* The compiler-facing resolution hook: where the patched FilPizlonator's
 * emitted call lands. It has to be native -- a `pizlonated_*` entry point is an
 * 8-byte descriptor stub, so calling it directly returns a function object
 * rather than running the body (see fasync_shared.h). Nothing here owns state:
 * fasync_published points at the memory-safe half's table and ring, published
 * once at ring setup. What lives here is the policy, because this is the only
 * place the compiler can reach. */
#include "fasync_shared.h"

static struct fasync_shared* volatile fasync_published;

/* How long to poll the CQ before agreeing to sleep. Polling is a plain read of
 * shared memory; parking is the expensive operation the design avoids. Must
 * match FASYNC_SPIN_LIMIT in fasync.c. */
#define FASYNC_NATIVE_SPIN_LIMIT 20000

/* Publish the shared state. Called once from the memory-safe half at ring setup
 * (the fasync_publish_state bridge). The address is retained, not copied, so
 * the memory it points into must not move: the ring is GC memory and the table
 * is static. stage2c_gc_pin_probe.c checks the former. */
PAS_API void filc_native_fasync_publish_state(filc_thread* my_thread,
                                              filc_ptr state) {
  PAS_UNUSED_PARAM(my_thread);
  fasync_published = (struct fasync_shared*)filc_ptr_ptr(state);
}

/* Reap whatever completions have landed: a pure userspace read of the shared
 * CQ -- no syscall, no mode switch. */
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
         * retired; it never sees a stale-reaped request as in flight. */
        __atomic_sub_fetch(sh->inflight, 1, __ATOMIC_RELEASE);
      }
    }
    count++;
  }

  *sh->local_cq_head = head;
  __atomic_store_n(sh->cq_head, head, __ATOMIC_RELEASE);
  *sh->completions_reaped += count;
}

/* The pending request covering [ptr, ptr+size), or 0. The walk lives in
 * fasync_shared.h so the two halves cannot drift apart (it used to be written
 * out once per toolchain, and the memo would have had to be kept in step by
 * hand); see there for why the memo exists and what proves it safe. */
static struct fasync_req_shared* fasync_native_find(struct fasync_shared* sh,
                                                    const void* ptr,
                                                    size_t size) {
  return fasync_shared_find(sh, ptr, size);
}

/* The lazy batch publish. The safe half can write SQEs the kernel never sees;
 * it only starts work once the SQ tail word is stored. This publishes
 * everything queued in one non-blocking enter -- what lets a workload written
 * as plain calls (a read_all_files loop) run with no submit step anywhere.
 * Reading the count non-atomically is fine: the resolver is only reached when
 * the acquire load of inflight was non-zero, which already ordered the stores
 * that bumped it. Draining by subtraction keeps a racing safe-side
 * fasync_submit() from re-publishing the same SQEs. */
static void fasync_native_submit(struct fasync_shared* sh) {
  if (!sh->sq_array)
    return;
  unsigned long n = __atomic_load_n(sh->queued, __ATOMIC_ACQUIRE);
  if (!n)
    return;

  unsigned int mask = *sh->sq_mask;
  unsigned int head = *sh->sqe_head;
  unsigned int tail = *sh->sqe_tail;
  for (unsigned int i = head; i != tail; i++)
    sh->sq_array[i & mask] = i & mask;

  /* The store that hands the batch to the kernel. */
  __atomic_store_n(sh->sq_tail, tail, __ATOMIC_RELEASE);
  *sh->sqe_head = tail;
  __atomic_sub_fetch(sh->queued, n, __ATOMIC_RELAXED);

  (*sh->kernel_submit_entries)++;
  fasync_syscall6(FASYNC_SYS_io_uring_enter, (long)sh->ring_fd, (long)n, 0L,
                  0L, 0L, 0L);
}

/* filc_resolve_pending -- what the compiler emits calls to.
 *
 * THE HOT PATH: it runs on every instrumented access, so its cost when nothing
 * is pending is the design's most important number -- one acquire load of a
 * cache-resident counter and a predictable branch. It spins against the
 * completion ring first and parks only second, because the common case is that
 * the completion has already landed. */
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

  /* Lazy batch publish: a workload written as plain calls queues SQEs with no
   * submit; this is where that batch first becomes visible to the kernel. */
  fasync_native_submit(sh);

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

  /* A full spin budget with no completion means sleeping is the only thing
   * left. This is the one call here that can block indefinitely, so it takes
   * the GC safepoint around the enter (same reason as above: a collector
   * handshake must not wait forever on a thread parked in the kernel). */
  while (r->state == FASYNC_REQ_PENDING) {
    (*sh->parks)++;
    (*sh->kernel_wait_entries)++;
    filc_thread* my_thread = filc_get_my_thread();
    if (my_thread)
      filc_exit(my_thread);
    fasync_syscall6(FASYNC_SYS_io_uring_enter, (long)sh->ring_fd, 0L, 1L,
                    FASYNC_ENTER_GETEVENTS, 0L, 0L);
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

/* Block until at least one completion is available, for operations that produce
 * a scalar (an fd, an error code) rather than filling a buffer -- those have no
 * range for the resolver to look up. Takes the GC safepoint: it can sleep
 * indefinitely. */
PAS_API void filc_native_fasync_block(filc_thread* my_thread) {
  struct fasync_shared* sh = fasync_published;
  if (!sh)
    return;
  (*sh->parks)++;
  (*sh->kernel_wait_entries)++;
  if (my_thread)
    filc_exit(my_thread);
  fasync_syscall6(FASYNC_SYS_io_uring_enter, (long)sh->ring_fd, 0L, 1L,
                  FASYNC_ENTER_GETEVENTS, 0L, 0L);
  if (my_thread)
    filc_enter(my_thread);
  fasync_native_drain(sh);
}