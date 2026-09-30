/* stage2b_mmap_probe.c -- kernel behaviour, plain C (deliberately not a Fil-C
 * program: it establishes a property of the kernel and of Fil-C's mmap wrapper).
 *
 * Fil-C's mmap wrapper must hand back a capability for the address a mapping
 * lands at, so it pre-allocates the address and passes MAP_FIXED. The kernel
 * rejects MAP_FIXED for io_uring ring mappings ((a) and (c) below), so a ring
 * can never carry a Fil-C capability -- which is why the runtime uses
 * IORING_SETUP_NO_MMAP and supplies ring memory itself, out of GC memory that
 * stage2c probes for stability. */

#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/syscall.h>

struct io_sqring_offsets {
  uint32_t head, tail, ring_mask, ring_entries, flags, dropped, array, resv1;
  uint64_t user_addr;
};
struct io_cqring_offsets {
  uint32_t head, tail, ring_mask, ring_entries, overflow, cqes, flags, resv1;
  uint64_t user_addr;
};
struct io_uring_params {
  uint32_t sq_entries, cq_entries, flags, sq_thread_cpu, sq_thread_idle,
      features, wq_fd, resv[3];
  struct io_sqring_offsets sq_off;
  struct io_cqring_offsets cq_off;
};

#define IORING_OFF_SQ_RING 0ULL
#define IORING_OFF_SQES 0x10000000ULL
#define IORING_FEAT_SINGLE_MMAP 1U

static int failures = 0;

static void check(const char* what, int ok) {
  printf("  %-58s %s\n", what, ok ? "ok" : "FAIL");
  if (!ok)
    failures++;
}

int main(void) {
  printf("io_uring ring mapping requirements:\n");

  struct io_uring_params p;
  memset(&p, 0, sizeof(p));

  int fd = (int)syscall(425, 16, &p);
  if (fd < 0) {
    printf("  io_uring_setup unavailable (%s); skipping\n", strerror(errno));
    return 0;
  }

  unsigned sq_sz = p.sq_off.array + p.sq_entries * 4;
  unsigned cq_sz = p.cq_off.cqes + p.cq_entries * 16;
  if (p.features & IORING_FEAT_SINGLE_MMAP) {
    if (cq_sz > sq_sz)
      sq_sz = cq_sz;
    cq_sz = sq_sz;
  }

  /* (a) The documented liburing pattern: NULL address, no MAP_FIXED. */
  errno = 0;
  void* a = mmap(NULL, sq_sz, PROT_READ | PROT_WRITE,
                 MAP_SHARED | MAP_POPULATE, fd, IORING_OFF_SQ_RING);
  check("mmap(NULL, ..., MAP_SHARED) succeeds", a != MAP_FAILED);
  if (a != MAP_FAILED)
    munmap(a, sq_sz);

  /* (b) The SQEs region maps the same way. */
  size_t sqes_sz = (size_t)p.sq_entries * 64;
  errno = 0;
  void* b = mmap(NULL, sqes_sz, PROT_READ | PROT_WRITE,
                 MAP_SHARED | MAP_POPULATE, fd, IORING_OFF_SQES);
  check("mmap of the SQEs region succeeds", b != MAP_FAILED);
  if (b != MAP_FAILED)
    munmap(b, sqes_sz);

  /*
   * (c) With MAP_FIXED the kernel refuses. This is the property that forces
   *     IORING_SETUP_NO_MMAP in our runtime, because Fil-C's mmap wrapper always
   *     passes MAP_FIXED when it allocates the address itself.
   */
  void* reserve = mmap(NULL, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (reserve != MAP_FAILED) {
    munmap(reserve, 4096);
    errno = 0;
    void* c = mmap(reserve, sq_sz, PROT_READ | PROT_WRITE,
                   MAP_SHARED | MAP_POPULATE | MAP_FIXED, fd, IORING_OFF_SQ_RING);
    check("mmap(addr, ..., MAP_FIXED) is rejected (EINVAL)",
          c == MAP_FAILED && errno == EINVAL);
  }

  close(fd);
  printf("\nSTAGE2B %s\n", failures ? "FAIL" : "PASS");
  return failures ? 1 : 0;
}
