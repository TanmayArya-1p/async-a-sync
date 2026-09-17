/*
 * stage6_fd_provenance.c -- provenance attached to descriptors.
 *
 * A buffer's provenance is a range: the request that fills it. A descriptor's
 * provenance is a slot: the request that will produce it. This test exercises the
 * second, which is the case idea.md section 3.1 calls out -- "the kernel needs
 * a's output as a literal SQE field for b" -- where the field is an fd.
 *
 * The whole point is what is NOT written here: the read is submitted against a
 * descriptor that does not exist at the time it is written, and nothing at the
 * call site says so. No handle, no await, no annotation. Passing the pending
 * descriptor to fasync_pread is what creates the dependency.
 *
 * Build:
 *   filcc -O2 -static -Iruntime/src -Lruntime/build/lib -o stage6 \
 *         tests/stage6_fd_provenance.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

#include "fasync.h"

#define BLOCK 8192

static int failures = 0;

static void check(const char* what, int ok) {
  printf("  %-58s %s\n", what, ok ? "ok" : "FAIL");
  fflush(stdout);
  if (!ok)
    failures++;
}

int main(void) {
  printf("descriptor provenance:\n");

  const char* path = "/tmp/async-a-sync_stage6_payload.bin";
  int fd = open(path, O_CREAT | O_TRUNC | O_RDWR, 0644);
  if (fd < 0) {
    printf("cannot create %s\n", path);
    return 1;
  }
  unsigned char* seed = malloc(BLOCK);
  memset(seed, 0x3C, BLOCK);
  pwrite(fd, seed, BLOCK, 0);
  free(seed);
  close(fd);

  unsigned char* buf = malloc(BLOCK);
  memset(buf, 0, BLOCK);

  /* ------------------------------------------------------------------ */
  /* 1. A pending descriptor is not a real descriptor.                  */
  /* ------------------------------------------------------------------ */
  fasync_reset_stats();

  int pending = fasync_open_pending(-100 /* AT_FDCWD */, path, O_RDONLY, 0);
  printf("  fasync_open_pending returned %d\n", pending);
  check("a pending descriptor is negative (so it cannot be a real fd)",
        pending < 0);
  check("and it is distinct from the failure value -1", pending != -1);

  /* ------------------------------------------------------------------ */
  /* 2. Using it is what creates the dependency -- with no submit() call. */
  /*                                                                     */
  /*    Resolving a pending descriptor is the ergonomic path, so it must  */
  /*    publish the queued open itself. Requiring the caller to remember  */
  /*    an fasync_submit() first would be a trap.                        */
  /* ------------------------------------------------------------------ */
  long resolved = fasync_fd_resolve(pending);
  printf("  fasync_fd_resolve returned %ld\n", resolved);
  check("it resolves to a real descriptor", resolved >= 0);

  /* ------------------------------------------------------------------ */
  /* 3. End to end: open and read, with the read written against a       */
  /*    descriptor that does not exist yet.                             */
  /* ------------------------------------------------------------------ */
  fasync_reset_stats();

  int pending2 = fasync_open_pending(-100, path, O_RDONLY, 0);
  fasync_id read_id = fasync_pread(pending2, buf, BLOCK, 0);
  check("a read can be submitted against a pending descriptor", read_id != 0);

  fasync_submit();

  long n = fasync_result(read_id);
  printf("  read returned %ld (expected %d)\n", n, BLOCK);

  int data_ok = 1;
  for (int i = 0; i < BLOCK; i += 256)
    if (buf[i] != 0x3C)
      data_ok = 0;

  check("the read returned the full block", n == BLOCK);
  check("the data is correct", data_ok);

  /* ------------------------------------------------------------------ */
  /* 4. The descriptor that came out of it is a normal descriptor.      */
  /* ------------------------------------------------------------------ */
  long real = fasync_fd_resolve(pending2);
  check("the pending descriptor still resolves to a usable fd", real >= 0);

  /* A synchronous read on it must work, proving it is a real descriptor and not
   * a direct-descriptor index. */
  char probe[8];
  ssize_t got = pread((int)real, probe, sizeof(probe), 0);
  check("a synchronous pread on the resolved fd works", got == (ssize_t)sizeof(probe));

  /* ------------------------------------------------------------------ */
  /* 5. A real descriptor passes through untouched.                     */
  /* ------------------------------------------------------------------ */
  check("fasync_fd_resolve passes a real fd through unchanged",
        fasync_fd_resolve(7) == 7);

  /* ------------------------------------------------------------------ */
  /* 6. An invalid handle is rejected rather than mistaken for an fd.   */
  /* ------------------------------------------------------------------ */
  check("an out-of-range pending handle is rejected",
        fasync_fd_resolve(-9999) < 0);

  free(buf);
  unlink(path);

  printf("\nSTAGE6 %s\n", failures ? "FAIL" : "PASS");
  return failures ? 1 : 0;
}
