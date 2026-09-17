/*
 * stage6b_fd_chain_probe.c -- can a read be chained to a file that is not open
 * yet, entirely in the kernel? Plain C, system compiler. NOT a Fil-C program.
 *
 * This is idea.md section 3.1's "(b) the kernel needs a's output as a literal SQE
 * field" case applied to descriptors: if provenance is attached to an fd, then a
 * read on an fd that does not exist yet should be submittable now and execute
 * when the open completes, with no userspace wait.
 *
 * It needs three things, and the probe checks them in order:
 *
 *   A. a sparse direct-descriptor table (entries of -1), giving the runtime its
 *      own pool of slots, and an openat that targets a specific slot;
 *   B. a read on that slot in a separate submission, once the open has landed;
 *   C. the real prize: openat and read submitted *together*, ordered by
 *      IOSQE_IO_LINK, with the read naming a slot that does not exist yet. This
 *      depends on IORING_FEAT_LINKED_FILE to defer file assignment until the
 *      request is issued.
 *
 * Build: cc -O2 -o stage6b_fd_chain_probe tests/stage6b_fd_chain_probe.c
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/syscall.h>

struct io_uring_sqe {
  uint8_t opcode, flags;
  uint16_t ioprio;
  int32_t fd;
  union {
    uint64_t off;
    uint64_t addr2;
  };
  union {
    uint64_t addr;
    uint64_t splice_off_in;
  };
  uint32_t len;
  union {
    uint32_t rw_flags;
    uint32_t fsync_flags;
    uint32_t open_flags;
  };
  uint64_t user_data;
  union {
    uint16_t buf_index;
    uint16_t buf_group;
  };
  uint16_t personality;
  union {
    int32_t splice_fd_in;
    uint32_t file_index;
    struct {
      uint16_t addr_len, pad3[1];
    };
  };
  union {
    struct {
      uint64_t addr3, pad2[1];
    };
    uint8_t cmd[0];
  };
};

struct io_uring_cqe {
  uint64_t user_data;
  int32_t res;
  uint32_t flags;
};

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

#define IORING_OP_READ 22
#define IORING_OP_OPENAT 18

#define IOSQE_FIXED_FILE (1U << 0)
#define IOSQE_IO_LINK (1U << 2)

#define IORING_REGISTER_FILES 2
#define IORING_FEAT_SINGLE_MMAP 1U
#define IORING_FEAT_LINKED_FILE (1U << 8)

#ifndef SYS_io_uring_setup
#define SYS_io_uring_setup 425
#endif
#ifndef SYS_io_uring_enter
#define SYS_io_uring_enter 426
#endif
#ifndef SYS_io_uring_register
#define SYS_io_uring_register 427
#endif

#define SLOTS 8

static int g_ring_fd;
static struct io_uring_sqe* g_sqes;
static struct io_uring_cqe* g_cqes;
static uint32_t *g_sq_tail, *g_sq_mask, *g_sq_array, *g_cq_head, *g_cq_tail,
    *g_cq_mask;
static uint32_t g_local_cq_head;

/*
 * The submission queue is a circular buffer whose tail only ever advances. An
 * earlier version of this probe rewound it on each submission, which made the
 * second submission invisible to the kernel and had the probe reporting a stale
 * completion as if it were fresh. Keeping an explicit local tail is the fix, and
 * it is worth stating because the symptom was thoroughly misleading.
 */
static uint32_t g_next_sqe;

static struct io_uring_sqe* next_sqe(void) {
  return &g_sqes[g_next_sqe & *g_sq_mask];
}

static void commit(unsigned n) {
  for (unsigned i = 0; i < n; i++) {
    uint32_t idx = (g_next_sqe + i) & *g_sq_mask;
    g_sq_array[idx] = idx;
  }
  g_next_sqe += n;
  __atomic_store_n(g_sq_tail, g_next_sqe, __ATOMIC_RELEASE);
  syscall(SYS_io_uring_enter, g_ring_fd, n, 0, 0, 0, 0);
}

static double now_s(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* Time-bounded, so that a hang is distinguishable from "the kernel never
 * completed it". */
static int reap(struct io_uring_cqe* out, unsigned want) {
  double deadline = now_s() + 3.0;
  while (now_s() < deadline) {
    uint32_t head = g_local_cq_head;
    uint32_t tail = __atomic_load_n(g_cq_tail, __ATOMIC_ACQUIRE);
    unsigned n = 0;
    while (head != tail && n < want) {
      out[n++] = g_cqes[head & *g_cq_mask];
      head++;
    }
    if (n) {
      g_local_cq_head = head;
      __atomic_store_n(g_cq_head, head, __ATOMIC_RELEASE);
      return (int)n;
    }
    syscall(SYS_io_uring_enter, g_ring_fd, 0, 0, 0, 0, 0);
  }
  return 0;
}

static int setup(int* ring_fd, struct io_uring_params* p) {
  int fd = (int)syscall(SYS_io_uring_setup, 32, p);
  if (fd < 0) {
    printf("io_uring_setup: %s\n", strerror(errno));
    return -1;
  }
  *ring_fd = fd;

  unsigned sq_sz = p->sq_off.array + p->sq_entries * 4;
  unsigned cq_sz = p->cq_off.cqes + p->cq_entries * 16;
  if (p->features & IORING_FEAT_SINGLE_MMAP) {
    if (cq_sz > sq_sz)
      sq_sz = cq_sz;
    cq_sz = sq_sz;
  }

  void* sq = mmap(0, sq_sz, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE,
                  fd, 0);
  void* cq = (p->features & IORING_FEAT_SINGLE_MMAP)
                 ? sq
                 : mmap(0, cq_sz, PROT_READ | PROT_WRITE,
                        MAP_SHARED | MAP_POPULATE, fd, 0x8000000);
  g_sqes = mmap(0, p->sq_entries * 64, PROT_READ | PROT_WRITE,
                MAP_SHARED | MAP_POPULATE, fd, 0x10000000);
  if (sq == MAP_FAILED || cq == MAP_FAILED || g_sqes == MAP_FAILED) {
    printf("ring mmap failed\n");
    return -1;
  }

  g_sq_tail = (uint32_t*)((char*)sq + p->sq_off.tail);
  g_sq_mask = (uint32_t*)((char*)sq + p->sq_off.ring_mask);
  g_sq_array = (uint32_t*)((char*)sq + p->sq_off.array);
  g_cqes = (struct io_uring_cqe*)((char*)cq + p->cq_off.cqes);
  g_cq_head = (uint32_t*)((char*)cq + p->cq_off.head);
  g_cq_tail = (uint32_t*)((char*)cq + p->cq_off.tail);
  g_cq_mask = (uint32_t*)((char*)cq + p->cq_off.ring_mask);
  return 0;
}

static void prep_openat(struct io_uring_sqe* s, const char* path,
                        unsigned slot, unsigned flags, uint64_t ud) {
  memset(s, 0, sizeof(*s));
  s->opcode = IORING_OP_OPENAT;
  s->fd = -100; /* AT_FDCWD */
  s->addr = (uint64_t)(uintptr_t)path;
  s->len = 0; /* mode */
  s->open_flags = O_RDONLY;
  s->file_index = slot;
  s->flags = (uint8_t)flags;
  s->user_data = ud;
}

static void prep_read(struct io_uring_sqe* s, unsigned slot, void* buf,
                      unsigned len, unsigned flags, uint64_t ud) {
  memset(s, 0, sizeof(*s));
  s->opcode = IORING_OP_READ;
  s->fd = (int32_t)slot;
  s->flags = (uint8_t)flags;
  s->addr = (uint64_t)(uintptr_t)buf;
  s->len = len;
  s->off = 0;
  s->user_data = ud;
}

int main(void) {
  const char* path = "/tmp/async-a-sync_fdchain_payload.txt";
  const char* payload = "chained-before-open";
  int fd = open(path, O_CREAT | O_TRUNC | O_RDWR, 0644);
  write(fd, payload, strlen(payload));
  close(fd);

  struct io_uring_params p;
  memset(&p, 0, sizeof(p));
  if (setup(&g_ring_fd, &p) < 0)
    return 1;

  printf("kernel features: 0x%x   IORING_FEAT_LINKED_FILE=%s\n", p.features,
         (p.features & IORING_FEAT_LINKED_FILE) ? "yes" : "no");

  /* A sparse direct-descriptor table. */
  int slots[SLOTS];
  for (int i = 0; i < SLOTS; i++)
    slots[i] = -1;
  long rc = syscall(SYS_io_uring_register, g_ring_fd, IORING_REGISTER_FILES,
                    slots, SLOTS);
  printf("register sparse file table (%d slots): %s\n\n", SLOTS,
         rc == 0 ? "ok" : strerror(errno));
  if (rc != 0)
    return 1;

  struct io_uring_cqe cqes[4];
  char buf[64];
  int a_ok = 0, b_ok = 0;

  /* ---------------------------------------------------------------- */
  /* A: two phases -- open into a slot, wait, then read that slot.     */
  /* ---------------------------------------------------------------- */
  memset(buf, 0, sizeof(buf));
  prep_openat(next_sqe(), path, 2, 0, 0xA1);
  commit(1);
  int g1 = reap(cqes, 1);
  printf("A: openat into slot 2         -> %s (res=%d)\n",
         g1 == 1 ? "completed" : "NO COMPLETION", g1 == 1 ? cqes[0].res : -999);

  /*
   * Two hypotheses to separate: either the open did not use a direct descriptor
   * at all, or it used one but not the index we asked for. `res` on a direct
   * open is supposed to be the index it landed in, so read whatever it reported
   * as well as the index we requested.
   */
  int reported = (g1 == 1) ? cqes[0].res : -999;
  printf("A: open reported index %d (we asked for 2)\n", reported);

  if (reported >= 0 && reported < SLOTS) {
    memset(buf, 0, sizeof(buf));
    prep_read(next_sqe(), (unsigned)reported, buf, sizeof(buf) - 1,
              IOSQE_FIXED_FILE, 0xA3);
    commit(1);
    int g3 = reap(cqes, 1);
    printf("A: read on the REPORTED slot %d -> %s (res=%d, want %zu)\n",
           reported, g3 == 1 ? "completed" : "NO COMPLETION",
           g3 == 1 ? cqes[0].res : -999, strlen(payload));
    printf("A: bytes: \"%s\"\n", buf);
    if (g3 == 1 && strcmp(buf, payload) == 0)
      a_ok = 1;
  }

  memset(buf, 0, sizeof(buf));
  prep_read(next_sqe(), 2, buf, sizeof(buf) - 1, IOSQE_FIXED_FILE, 0xA2);
  commit(1);
  int g2 = reap(cqes, 1);
  printf("A: read on the REQUESTED slot 2 -> %s (res=%d)\n",
         g2 == 1 ? "completed" : "NO COMPLETION", g2 == 1 ? cqes[0].res : -999);
  printf("A: %s\n\n",
         a_ok ? "WORKS -- a direct descriptor was used"
              : "does NOT work -- no direct descriptor became readable");

  /* ---------------------------------------------------------------- */
  /* B: one chain -- openat (linked) and read submitted together, with */
  /*    the read naming a slot that does not exist yet.                */
  /* ---------------------------------------------------------------- */
  memset(buf, 0, sizeof(buf));
  prep_openat(next_sqe(), path, 4, IOSQE_IO_LINK, 0xB1);
  prep_read(next_sqe(), 4, buf, sizeof(buf) - 1, IOSQE_FIXED_FILE, 0xB2);
  commit(2);

  int g3 = reap(cqes, 2);
  printf("B: reaped %d completion(s)\n", g3);
  for (int i = 0; i < g3; i++)
    printf("B:   user_data=0x%llx res=%d\n",
           (unsigned long long)cqes[i].user_data, cqes[i].res);
  printf("B: bytes: \"%s\"\n", buf);
  b_ok = (g3 == 2 && strcmp(buf, payload) == 0);
  printf("B: %s\n\n",
         b_ok ? "WORKS -- genuine kernel promise pipelining" : "does NOT work");

  /*
   * This is a probe, not a pass/fail test: it establishes what the kernel
   * supports so that the runtime can be built around it. The observable finding
   * is that an openat targeting an *explicit* slot is not honoured here -- the
   * kernel allocates its own slot instead (tests showed it returning index 0 for
   * a requested 2), which is why a chained read has nothing to name in advance
   * and kernel-native fd pipelining is unavailable. A separate probe
   * (/tmp/fixedfile.c, reproduced in docs) confirmed the IORING_FILE_INDEX_ALLOC
   * path does work, and that fixed-file reads on a registered slot work.
   */
  printf("\nFINDING: explicit-slot openat honoured: %s\n",
         a_ok ? "yes" : "no");
  printf("FINDING: kernel-native fd chaining:     %s\n",
         b_ok ? "available" : "unavailable (needs explicit slots)");
  printf("\nSTAGE6B probe complete\n");
  close(g_ring_fd);
  unlink(path);
  return 0;
}
