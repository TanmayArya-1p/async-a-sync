/* async_io: the explicit fasync API on one file of 64 blocks.
 *
 *   lazy           queue every read; reading a block waits for it
 *   one at a time  the same reads with blocking pread
 *   all at once    the same reads, all queued, then waited for
 *   dependencies   which of four operations must wait for which */

#include "explicit_utils.hh"

// Queue every read. Checking a block waits for it.
static int lazy(int fd, unsigned char** block) {
  fasync_id id[BLOCKS];
  for (int i = 0; i < BLOCKS; i++)
    id[i] = fasync_pread(fd, block[i], BLOCK_SIZE, (unsigned long)i * BLOCK_SIZE);
  int ok = 1;
  for (int i = 0; i < BLOCKS; i++)
    ok &= block_ok(block[i], i) &&            // waits for that read
          fasync_result(id[i]) == BLOCK_SIZE; // reaps the handle
  return ok;
}

// One blocking read after another.
static int one_at_a_time(int fd, unsigned char** block) {
  int ok = 1;
  for (int i = 0; i < BLOCKS; i++)
    ok &= pread(fd, block[i], BLOCK_SIZE, (off_t)i * BLOCK_SIZE) == BLOCK_SIZE &&
          block_ok(block[i], i);
  return ok;
}

// Queue every read, then wait for each.
static int all_at_once(int fd, unsigned char** block) {
  fasync_id id[BLOCKS];
  for (int i = 0; i < BLOCKS; i++)
    id[i] = fasync_pread(fd, block[i], BLOCK_SIZE, (unsigned long)i * BLOCK_SIZE);
  int ok = 1;
  for (int i = 0; i < BLOCKS; i++)
    ok &= fasync_result(id[i]) == BLOCK_SIZE && block_ok(block[i], i);
  return ok;
}

// Four operations on buffers a, b and c. Which must wait for which?
static void dependencies(void) {
  static char a[1024], b[1024], c[1024];
  struct fasync_access write_a[] = {FASYNC_ACCESS_RANGE(a, 1024, FASYNC_OUT)};
  struct fasync_access write_b[] = {FASYNC_ACCESS_RANGE(b, 1024, FASYNC_OUT)};
  struct fasync_access copy_a_c[] = {FASYNC_ACCESS_RANGE(a, 1024, FASYNC_IN),
                                     FASYNC_ACCESS_RANGE(c, 1024, FASYNC_OUT)};
  struct fasync_access read_c[] = {FASYNC_ACCESS_RANGE(c, 1024, FASYNC_IN)};
  struct fasync_op op[4] = {
      {"write(a)", write_a, 1},
      {"write(b)", write_b, 1},
      {"copy(a,c)", copy_a_c, 2},
      {"read(c)", read_c, 1},
  };

  unsigned edge[16];
  struct fasync_dag_stats st;
  unsigned n = fasync_build_dag(op, 4, edge, 16, &st);
  show_dag(op, 4, edge, n, &st);
}

int main(int argc, char** argv) {
  struct blocks b = async_io_setup(argc, argv);

  step(&b, "lazy: queue all, check each", lazy(b.fd, b.block));
  step(&b, "one blocking pread at a time", one_at_a_time(b.fd, b.block));
  step(&b, "all queued, then waited for", all_at_once(b.fd, b.block));
  dependencies();

  return async_io_report(&b);
}
