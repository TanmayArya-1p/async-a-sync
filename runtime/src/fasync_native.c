#include "filc_runtime.h"

#include "fasync_io_uring.h"

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

/* raw syscalls return -errno so convert to -1 */
static PAS_ALWAYS_INLINE long fasync_finish(long ret) {
  if (ret < 0 && ret >= -4095) {
    filc_set_errno((int)-ret);
    return -1;
  }
  return ret;
}

PAS_API long filc_native_zsys_io_uring_setup(filc_thread* my_thread,
                                             unsigned entries,
                                             filc_ptr params) {
  PAS_UNUSED_PARAM(my_thread);
  filc_check_write(params, sizeof(struct fasync_params));
  return fasync_finish(
      fasync_syscall2(FASYNC_SYS_io_uring_setup, (long)entries,
                      (long)filc_ptr_ptr(params)));
}

/* submit-only enter no safepoint blocking enter leaves first */
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

/* hook native because pizlonated entry is a stub */
#include "fasync_shared.h"

static struct fasync_shared* volatile fasync_published;

#define FASYNC_NATIVE_SPIN_LIMIT 20000

PAS_API void filc_native_fasync_publish_state(filc_thread* my_thread,
                                              filc_ptr state) {
  PAS_UNUSED_PARAM(my_thread);
  fasync_published = (struct fasync_shared*)filc_ptr_ptr(state);
}

/* userspace cq read no syscall */
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
        /* retire only after the final state is visible */
        __atomic_sub_fetch(sh->inflight, 1, __ATOMIC_RELEASE);
      }
    }
    count++;
  }

  *sh->local_cq_head = head;
  __atomic_store_n(sh->cq_head, head, __ATOMIC_RELEASE);
  *sh->completions_reaped += count;
}

static struct fasync_req_shared* fasync_native_find(struct fasync_shared* sh,
                                                    const void* ptr,
                                                    size_t size) {
  return fasync_shared_find(sh, ptr, size);
}

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

  /* the store that hands the batch to the kernel */
  __atomic_store_n(sh->sq_tail, tail, __ATOMIC_RELEASE);
  *sh->sqe_head = tail;
  __atomic_sub_fetch(sh->queued, n, __ATOMIC_RELAXED);

  (*sh->kernel_submit_entries)++;
  fasync_syscall6(FASYNC_SYS_io_uring_enter, (long)sh->ring_fd, (long)n, 0L,
                  0L, 0L, 0L);
}

/* hot path runs on every access */
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

  /* lazy publish makes the batch visible */
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

  /* spin budget out so sleep with the safepoint */
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

PAS_API void filc_native_fasync_poll(filc_thread* my_thread) {
  PAS_UNUSED_PARAM(my_thread);
  fasync_native_drain(fasync_published);
}

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