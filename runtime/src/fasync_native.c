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

/* The completion ring is drained natively: the Fil-C half cannot map the
 * kernel's ring memory with its own capabilities. */
#include "fasync_shared.h"

static struct fasync_shared* volatile fasync_published;

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
