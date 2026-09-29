#pragma once

/* Setup, output and checks for demo_rpc_upload, so the demo's source holds
 * only the loop it demonstrates. */

#include <stdint.h>
#include <stdlib.h>

#include "rpc_upload.hh"

#define UPLOAD_FILES 4
#define UPLOAD_BYTES 4096

struct upload {
  unsigned port;
  char path[UPLOAD_FILES][256];
  int fd[UPLOAD_FILES];
  char* buf[UPLOAD_FILES];
  uint32_t expect[UPLOAD_FILES]; /* checksum of each file's bytes */
  struct pragma_snap start;
  double t0, loop_ms;
};

/* The server's checksum: FNV-1a over the bytes. Byte by byte in compiled
 * code, so reading a pending buffer waits for it. */
static inline uint32_t upload_checksum(const char* p, size_t len) {
  uint32_t hash = 2166136261u;
  for (size_t i = 0; i < len; i++)
    hash = (hash ^ (unsigned char)p[i]) * 16777619u;
  return hash;
}

/* Writes UPLOAD_FILES files of distinct text, drops them from the page cache
 * so each read is a device round trip, and opens them. */
static inline struct upload upload_setup(int argc, char** argv) {
  struct upload u = {0};
  unsigned long port = argc > 2 ? strtoul(argv[1], NULL, 10) : 0;
  if (port == 0 || port > 65535) {
    fprintf(stderr, "usage: demo_rpc_upload PORT DIR\n");
    exit(2);
  }
  u.port = (unsigned)port;
  pragma_title("upload: read files with io_uring, upload them with rpc",
               "  async_pread() says runtime=io_uring; upload() says runtime=rpc.");

  char text[UPLOAD_BYTES];
  for (int i = 0; i < UPLOAD_FILES; i++) {
    for (size_t j = 0; j < UPLOAD_BYTES; j++)
      text[j] = "abcdefghijklmnopqrstuvwxyz\n"[(j * (i + 3)) % 27];
    u.expect[i] = upload_checksum(text, UPLOAD_BYTES);
    snprintf(u.path[i], sizeof(u.path[i]), "%s/demo_rpc_upload_%d.txt", argv[2], i);
    u.fd[i] = open(u.path[i], O_CREAT | O_TRUNC | O_RDWR, 0644);
    if (u.fd[i] < 0 || pwrite(u.fd[i], text, UPLOAD_BYTES, 0) != UPLOAD_BYTES ||
        fsync(u.fd[i]) != 0) {
      perror(u.path[i]);
      exit(1);
    }
    posix_fadvise(u.fd[i], 0, UPLOAD_BYTES, POSIX_FADV_DONTNEED);
    u.buf[i] = (char*)calloc(1, UPLOAD_BYTES);
  }
  pragma_snap(&u.start);
  u.t0 = demo_now_ms();
  return u;
}

static inline void upload_issued(struct upload* u) {
  u->loop_ms = demo_now_ms() - u->t0;
}

static inline int upload_report(struct upload* u, void** sent) {
  long reply[UPLOAD_FILES];
  for (int i = 0; i < UPLOAD_FILES; i++)
    reply[i] = pragma_wait(sent[i]);
  double total_ms = demo_now_ms() - u->t0;
  struct pragma_snap used = pragma_since(&u->start);

  int matched = 0;
  printf("  %-6s %-6s %-18s %s\n", "file", "bytes", "server checksum",
         "file checksum");
  for (int i = 0; i < UPLOAD_FILES; i++) {
    uint32_t mine = upload_checksum(u->buf[i], UPLOAD_BYTES);
    printf("  %-6d %-6d 0x%08lx         0x%08x\n", i, UPLOAD_BYTES,
           (unsigned long)reply[i], u->expect[i]);
    matched += reply[i] == (long)u->expect[i] && mine == u->expect[i];
  }
  printf("\n  the loop returned in %.2f ms; the last upload was answered after %.1f ms\n",
         u->loop_ms, total_ms);
  printf("  the %lu reads reached the kernel in %lu submit\n", used.sqes,
         used.submits);
  printf("\n  => each upload waited for its own read, and the reads still went out"
         " as one batch\n");

  pragma_check("every upload sent the bytes its read produced",
               matched == UPLOAD_FILES);
  pragma_check("the reads reached the kernel in one submit",
               used.sqes == UPLOAD_FILES && used.submits == 1);
  pragma_check("every call completed without failing",
               used.calls == 2 * UPLOAD_FILES &&
                   used.completed == 2 * UPLOAD_FILES && used.failed == 0);

  for (int i = 0; i < UPLOAD_FILES; i++) {
    close(u->fd[i]);
    unlink(u->path[i]);
    free(u->buf[i]);
  }
  return pragma_finish();
}
