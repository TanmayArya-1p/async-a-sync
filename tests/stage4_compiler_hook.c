/* stage4_compiler_hook.c -- proves the patched FilPizlonator inserts the hook.
 *
 * Built with the patched compiler and -DFASYNC_COMPILER_INSERTS_CHECKS,
 * FASYNC_ACCESS() expands to nothing and fasync_result() is never called, so
 * the only thing that can resolve a pending request is a call the compiler
 * inserted. Sampling the resolve counter across a plain load shows it firing.
 * Build and run see tests/run.sh. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

#include "fasync.h"

#ifndef FASYNC_COMPILER_INSERTS_CHECKS
#error "build this test with the patched compiler and -DFASYNC_COMPILER_INSERTS_CHECKS"
#endif

#define BLOCK 4096

int main(void) {
  printf("compiler-inserted resolution hook:\n");

  const char* path = "/tmp/async-a-sync_stage4_payload.bin";
  int fd = open(path, O_CREAT | O_TRUNC | O_RDWR, 0644);
  if (fd < 0) {
    printf("  cannot create %s\n", path);
    return 1;
  }
  unsigned char* seed = malloc(BLOCK);
  memset(seed, 0x5A, BLOCK);
  if (pwrite(fd, seed, BLOCK, 0) != BLOCK) {
    printf("  cannot seed\n");
    return 1;
  }
  free(seed);
  close(fd);

  fd = open(path, O_RDONLY);
  if (fd < 0)
    return 1;

  /* Escaping pointer: what the pass instruments. */
  unsigned char* buf = malloc(BLOCK);
  memset(buf, 0, BLOCK);

  fasync_reset_stats();

  fasync_id id = fasync_pread(fd, buf, BLOCK, 0);
  if (!id) {
    printf("  enqueue failed: %s\n", fasync_last_error());
    return 1;
  }
  int submitted = fasync_submit();

  {
    struct fasync_stats s0;
    fasync_get_stats(&s0);
    printf("  after submit: returned=%d sqes_queued=%lu inflight-still-pending=%s\n",
           submitted, s0.sqes_queued, s0.completions_reaped ? "no" : "yes");
  }

  /* No fasync_result(), no FASYNC_ACCESS(): an ordinary access through an
   * escaping pointer. Whatever resolved it was the compiler's instrumentation.
   */
  struct fasync_stats before, after;
  fasync_get_stats(&before);

  unsigned sum = 0;
  for (int i = 0; i < BLOCK; i++)
    sum += buf[i];

  fasync_get_stats(&after);

  printf("  resolve calls attributed to the access: before=%lu after=%lu\n",
         before.resolve_calls, after.resolve_calls);
  printf("  fast-path hits (means 'nothing was in flight'): %lu\n",
         after.fast_path_hits);
  printf("  kernel wait entries: %lu, submit entries: %lu\n",
         after.kernel_wait_entries, after.kernel_submit_entries);
  printf("  bytes read: %u (expected %u)\n", sum, 0x5Au * BLOCK);

  int hook_fired = after.resolve_calls > before.resolve_calls;
  int data_ok = (sum == 0x5Au * BLOCK);
  int reaped = after.completions_reaped > 0;

  printf("  hook fired at the access:    %s\n", hook_fired ? "yes" : "NO");
  printf("  completion reaped:           %s\n", reaped ? "yes" : "NO");
  printf("  buffer contents correct:     %s\n", data_ok ? "yes" : "NO");

  int ok = hook_fired && data_ok && reaped;
  free(buf);
  close(fd);
  unlink(path);

  printf("\nSTAGE4 %s\n", ok ? "PASS" : "FAIL");
  return ok ? 0 : 1;
}
