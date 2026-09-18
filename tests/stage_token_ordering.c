/*
 * stage_token_ordering.c -- a provenance tag orders two calls with no visible
 * dataflow connection.
 *
 * A write and a read of the same region, expressed as two separate buffers:
 * nothing in the dataflow ties them together, so there is no dependency. The
 * tag -- async-a-sync.pdf's serialization token, via the tracking API -- is the
 * ordering: the second call cannot issue its syscall until the first has
 * finished. The control is the same pair without the tag, which the runtime
 * proves is unheld by design by showing neither request has completed.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

#include "fasync.h"
#include "fasync_dep.h"

static int failures = 0;

static void check(const char* what, int ok) {
  printf("  %-58s %s\n", what, ok ? "ok" : "FAIL");
  fflush(stdout);
  if (!ok)
    failures++;
}

#define BLOCK 65536

int main(void) {
  const char* path = "/tmp/async-a-sync_stage_token.bin";
  int fd = open(path, O_CREAT | O_TRUNC | O_RDWR, 0644);
  if (fd < 0) {
    printf("cannot create %s\n", path);
    return 1;
  }

  /* What is already on disk: the "stale" bytes a racing read would fetch. */
  unsigned char* stale = malloc(BLOCK);
  memset(stale, 0x5A, BLOCK);
  if (pwrite(fd, stale, BLOCK, 0) != BLOCK)
    return 1;

  /* Control -- the same pair without the tag. */
  {
    unsigned char* src = malloc(BLOCK);
    memset(src, 0x3C, BLOCK);
    unsigned char* dst = malloc(BLOCK);
    memset(dst, 0, BLOCK);

    fasync_reset_stats();
    fasync_id w = fasync_pwrite(fd, src, BLOCK, 0);
    fasync_id r = fasync_pread(fd, dst, BLOCK, 0);
    check("untagged: both requests created", w && r);
    check("untagged: write not yet handed to the kernel",
          fasync_ready(w) == 0);
    check("untagged: read not held by the write", fasync_ready(r) == 0);
    check("untagged: write completed", fasync_result(w) == BLOCK);
    check("untagged: read completed", fasync_result(r) == BLOCK);

    /* Put the stale bytes back so the tagged half starts clean. */
    if (pwrite(fd, stale, BLOCK, 0) != BLOCK)
      return 1;
    free(src);
    free(dst);
  }

  /* The tag -- the read cannot even be issued until the write has finished. */
  {
    unsigned char* src = malloc(BLOCK);
    memset(src, 0x3C, BLOCK);
    unsigned char* dst = malloc(BLOCK);
    memset(dst, 0, BLOCK);

    struct fasync_tracker* tag = fasync_tracker_new();
    if (!tag) {
      printf("  fasync_tracker_new failed\n");
      return 1;
    }

    fasync_id w = fasync_tagged_pwrite(fd, src, BLOCK, 0, tag, FASYNC_INOUT);
    fasync_id r = fasync_tagged_pread(fd, dst, BLOCK, 0, tag, FASYNC_INOUT);
    check("tagged: both issued", w && r);
    check("tagged: issuing the read forced the write to finish",
          fasync_ready(w));
    check("tagged: writer's result", fasync_result(w) == BLOCK);
    check("tagged: reader's result", fasync_result(r) == BLOCK);
    check("tagged: reader saw exactly the author's bytes",
          memcmp(src, dst, BLOCK) == 0);

    free(src);
    free(dst);
    fasync_tracker_free(tag);
  }

  /* A writer must also wait for a prior reader (anti-dependency), so the order
   * holds either way around. */
  {
    unsigned char* dst = malloc(BLOCK);
    memset(dst, 0, BLOCK);
    unsigned char* src = malloc(BLOCK);
    memset(src, 0xCC, BLOCK);

    struct fasync_tracker* tag = fasync_tracker_new();

    fasync_id r = fasync_tagged_pread(fd, dst, BLOCK, 0, tag, FASYNC_IN);
    fasync_id w = fasync_tagged_pwrite(fd, src, BLOCK, 0, tag, FASYNC_OUT);
    check("reverse: issuing the writer forced the reader to finish",
          fasync_ready(r));
    check("reverse: writer's result", fasync_result(w) == BLOCK);
    check("reverse: reader's result", fasync_result(r) == BLOCK);

    free(src);
    free(dst);
    fasync_tracker_free(tag);
  }

  /* A chain of three writers: each waits for its predecessor, so issuing the
   * third proves the first two have already landed. */
  {
    unsigned char* one = malloc(BLOCK);
    memset(one, 0x11, BLOCK);
    unsigned char* two = malloc(BLOCK);
    memset(two, 0x22, BLOCK);
    unsigned char* thr = malloc(BLOCK);
    memset(thr, 0x33, BLOCK);

    struct fasync_tracker* tag = fasync_tracker_new();

    fasync_id a = fasync_tagged_pwrite(fd, one, BLOCK, 0, tag, FASYNC_INOUT);
    fasync_id b = fasync_tagged_pwrite(fd, two, BLOCK, 0, tag, FASYNC_INOUT);
    fasync_id c = fasync_tagged_pwrite(fd, thr, BLOCK, 0, tag, FASYNC_INOUT);
    check("chain: first write done before the third was issued", fasync_ready(a));
    check("chain: second write done before the third was issued", fasync_ready(b));
    check("chain: all three results", fasync_result(a) == BLOCK &&
                                         fasync_result(b) == BLOCK &&
                                         fasync_result(c) == BLOCK);

    unsigned char* back = malloc(BLOCK);
    memset(back, 0, BLOCK);
    if (pread(fd, back, BLOCK, 0) != BLOCK)
      return 1;
    check("chain: the file ends with the last writer's bytes",
          memcmp(back, thr, BLOCK) == 0);
    free(back);
    free(one);
    free(two);
    free(thr);
    fasync_tracker_free(tag);
  }

  close(fd);
  unlink(path);
  free(stale);

  printf("\nSTAGE-TOKEN %s\n", failures ? "FAIL" : "PASS");
  return failures ? 1 : 0;
}