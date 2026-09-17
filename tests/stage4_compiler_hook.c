/*
 * stage4_compiler_hook.c -- proves the patched FilPizlonator inserts the hook.
 *
 * This test is only meaningful when built with the *patched* compiler and with
 * -DFASYNC_COMPILER_INSERTS_CHECKS, which makes the FASYNC_ACCESS() macro expand
 * to nothing. Under those conditions this program contains no explicit
 * resolution call anywhere: not the macro (compiled away), and not
 * fasync_result() (never called).
 *
 * So the only thing that can resolve a pending request is a call the *compiler*
 * inserted. The test detects that by sampling the runtime's resolve counter
 * immediately before and after a plain memory access:
 *
 *     fasync_get_stats(&before);
 *     sum += buf[i];              <- an ordinary load
 *     fasync_get_stats(&after);
 *
 * If the counter moved, the hook fired at the access. This is deterministic: it
 * does not depend on whether the kernel has finished the read yet, because the
 * request stays in flight in the runtime's table until someone reaps its
 * completion, and the inserted call is what reaps it.
 *
 * Build (see tests/run.sh):
 *   <patched>/filcc -O2 -static -DFASYNC_COMPILER_INSERTS_CHECKS \
 *     -I runtime/src -L runtime/build/lib -o stage4 tests/stage4_compiler_hook.c
 */

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

  /* Heap buffer: an escaping pointer, which is what the pass instruments. */
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

  /*
   * No fasync_result(), no FASYNC_ACCESS(). Just an ordinary access through an
   * escaping pointer. If the compiler instrumented it, this resolves; if it did
   * not, nothing does.
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
