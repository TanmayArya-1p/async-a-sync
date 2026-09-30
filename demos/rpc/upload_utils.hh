#pragma once

// Files, checksums, setup and report for demo_rpc_upload.

#include <stdint.h>

#include "fasync.h"
#include "rpc_utils.hh"

#define UPLOAD_FILES 4
#define UPLOAD_BYTES 4096

struct demo {
  unsigned port;
  char path[UPLOAD_FILES][256];
  int fd[UPLOAD_FILES];
  char* buf[UPLOAD_FILES];
  uint32_t expect[UPLOAD_FILES]; // checksum of each file's bytes
  filc_async_stats start;
  struct fasync_stats io_start;
};

// The server's checksum: FNV-1a over the bytes. Compiled code, so reading
// a pending buffer waits for it.
static inline uint32_t upload_checksum(const char* p, size_t len) {
  uint32_t hash = 2166136261u;
  for (size_t i = 0; i < len; i++)
    hash = (hash ^ (unsigned char)p[i]) * 16777619u;
  return hash;
}

// Writes UPLOAD_FILES files of distinct text into DIR, dropped from the
// page cache, and opens them.
static inline struct demo setup(int argc, char** argv) {
  struct demo d = {0};
  d.port = port_arg(argc, argv, "demo_rpc_upload PORT DIR");
  const char* dir = argc > 2 ? argv[2] : "/tmp";
  title("upload: read files with io_uring, upload them with rpc",
        "  async_pread() says runtime=io_uring; upload() says runtime=rpc.");

  char text[UPLOAD_BYTES];
  for (int i = 0; i < UPLOAD_FILES; i++) {
    for (size_t j = 0; j < UPLOAD_BYTES; j++)
      text[j] = "abcdefghijklmnopqrstuvwxyz\n"[(j * (i + 3)) % 27];
    d.expect[i] = upload_checksum(text, UPLOAD_BYTES);
    snprintf(d.path[i], sizeof(d.path[i]), "%s/demo_rpc_upload_%d.txt", dir, i);
    d.fd[i] = open(d.path[i], O_CREAT | O_TRUNC | O_RDWR, 0644);
    if (d.fd[i] < 0 || pwrite(d.fd[i], text, UPLOAD_BYTES, 0) != UPLOAD_BYTES ||
        fsync(d.fd[i]) != 0) {
      perror(d.path[i]);
      exit(1);
    }
    posix_fadvise(d.fd[i], 0, UPLOAD_BYTES, POSIX_FADV_DONTNEED);
    d.buf[i] = (char*)calloc(1, UPLOAD_BYTES);
  }
  filc_async_get_stats(&d.start);
  fasync_get_stats(&d.io_start);
  calls.t0 = now_ms();
  return d;
}

// Waits for every upload and checks the server's checksums.
static inline int report(struct demo* d, void** sent) {
  long reply[UPLOAD_FILES];
  for (int i = 0; i < UPLOAD_FILES; i++)
    reply[i] = wait_for(sent[i]);
  double total_ms = now_ms() - calls.t0;
  double loop_ms = calls.n ? calls.ms[(calls.n < LOG_MAX ? calls.n : LOG_MAX) - 1] : 0;
  filc_async_stats s;
  struct fasync_stats io;
  filc_async_get_stats(&s);
  fasync_get_stats(&io);
  unsigned long reads = io.sqes_queued - d->io_start.sqes_queued;
  unsigned long submits = io.kernel_submit_entries - d->io_start.kernel_submit_entries;

  int matched = 0;
  printf("  %-6s %-6s %-18s %s\n", "file", "bytes", "server checksum",
         "file checksum");
  for (int i = 0; i < UPLOAD_FILES; i++) {
    uint32_t mine = upload_checksum(d->buf[i], UPLOAD_BYTES);
    printf("  %-6d %-6d 0x%08lx         0x%08x\n", i, UPLOAD_BYTES,
           (unsigned long)reply[i], d->expect[i]);
    matched += reply[i] == (long)d->expect[i] && mine == d->expect[i];
  }
  printf("\n  the loop's last call ran at %.2f ms; the last upload was answered "
         "after %.1f ms\n",
         loop_ms, total_ms);
  printf("  the %lu reads reached the kernel in %lu submit\n", reads, submits);
  printf("\n  => each upload waited for its own read, and the reads still went out"
         " as one batch\n");

  unsigned long made = s.tasks_submitted - d->start.tasks_submitted;
  unsigned long completed = s.tasks_completed - d->start.tasks_completed;
  unsigned long failed = s.tasks_failed - d->start.tasks_failed;
  check("every upload sent the bytes its read produced", matched == UPLOAD_FILES);
  check("the reads reached the kernel in one submit",
        reads == UPLOAD_FILES && submits == 1);
  check("every call completed without failing",
        made == 2 * UPLOAD_FILES && completed == 2 * UPLOAD_FILES && failed == 0);

  for (int i = 0; i < UPLOAD_FILES; i++) {
    close(d->fd[i]);
    unlink(d->path[i]);
    free(d->buf[i]);
  }
  return finish();
}
