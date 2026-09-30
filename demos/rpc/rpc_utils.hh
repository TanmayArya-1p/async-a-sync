#pragma once

// Logging, setup and reports for the rpc demos, so each demo file holds
// only its annotated functions and their calls.

#include <stdint.h>

#include "fasync.h"
#include "filc_async.h"
#include "../common/utils.hh"

// ---- the call log ----
// Each annotated body calls log_call. The runtime runs a body just before
// it sends the call.

#define LOG_MAX 16
static struct {
  int n;
  double t0;
  const char* name[LOG_MAX];
  double ms[LOG_MAX]; // since setup
} calls __attribute__((unused));

static inline void log_call(const char* name) {
  if (calls.n < LOG_MAX) {
    calls.name[calls.n] = name;
    calls.ms[calls.n] = now_ms() - calls.t0;
  }
  calls.n++;
}

// Waits for an annotated call; returns its result (-errno on failure).
static inline long wait_for(void* task) {
  struct filc_async_result_s r = {0};
  r.pending = task;
  filc_async_wait(&r);
  return r.result;
}

static inline unsigned port_arg(int argc, char** argv, const char* usage) {
  unsigned long port = argc > 1 ? strtoul(argv[1], NULL, 10) : 0;
  if (port == 0 || port > 65535) {
    fprintf(stderr, "usage: %s\n", usage);
    exit(2);
  }
  return (unsigned)port;
}

// ---- demo_rpc_counter ----

#define REPLY_MS 50 // the server's delay per reply

static inline unsigned counter_setup(int argc, char** argv) {
  unsigned port = port_arg(argc, argv, "demo_rpc_counter PORT");
  title("rpc: calls to a TCP counter server that read like plain C",
        "  The server takes 50 ms per reply. No call is waited for:\n"
        "  reading a value waits for its reply.");
  calls.t0 = now_ms();
  return port;
}

// The four values in call order, already read by main.
static inline int counter_report(long first, long second, long stepped,
                                 long after) {
  double read_ms = now_ms() - calls.t0;
  long value[4] = {first, second, stepped, after};
  filc_async_stats stats;
  filc_async_get_stats(&stats);

  printf("  %-6s %9s  %5s\n", "call", "sent at", "value");
  for (int i = 0; i < 4 && i < calls.n; i++)
    printf("  %-6s %6.0f ms  %5ld\n", calls.name[i], calls.ms[i], value[i]);
  printf("  %-6s %6.0f ms\n", "read", read_ms);

  // A call that waited was sent about one reply after the one before it.
  int step_waited = calls.ms[2] - calls.ms[1] > 0.8 * REPLY_MS;
  int get_waited = calls.ms[3] - calls.ms[2] > 0.8 * REPLY_MS;
  printf("\n  => step %s for both gets, and the last get %s for the step.\n"
         "     4 calls in %.0f ms; one at a time they take %d ms.\n\n",
         step_waited ? "waited" : "did not wait",
         get_waited ? "waited" : "did not wait", read_ms, 4 * REPLY_MS);

  check("the calls were sent as get, get, step, get",
        calls.n == 4 && strcmp(calls.name[2], "step") == 0);
  check("the gets before the step read 0", first == 0 && second == 0);
  check("the step made it 1, and the get after it read 1",
        stepped == 1 && after == 1);
  check("only the step and the get after it waited for a lock",
        stats.lock_waits == 2);
  check("the two gets ran together: step was sent within one reply",
        calls.ms[2] < 1.8 * REPLY_MS);
  check("reading the values waited for the last reply",
        read_ms - calls.ms[3] > 0.8 * REPLY_MS);
  check("every call completed without failing",
        stats.tasks_submitted == 4 && stats.tasks_completed == 4 &&
            stats.tasks_failed == 0);
  return finish();
}

// ---- demo_rpc_upload ----

#define UPLOAD_FILES 4
#define UPLOAD_BYTES 4096

struct upload {
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
static inline struct upload upload_setup(int argc, char** argv) {
  struct upload u = {0};
  u.port = port_arg(argc, argv, "demo_rpc_upload PORT DIR");
  const char* dir = argc > 2 ? argv[2] : "/tmp";
  title("upload: read files with io_uring, upload them with rpc",
        "  async_pread() says runtime=io_uring; upload() says runtime=rpc.");

  char text[UPLOAD_BYTES];
  for (int i = 0; i < UPLOAD_FILES; i++) {
    for (size_t j = 0; j < UPLOAD_BYTES; j++)
      text[j] = "abcdefghijklmnopqrstuvwxyz\n"[(j * (i + 3)) % 27];
    u.expect[i] = upload_checksum(text, UPLOAD_BYTES);
    snprintf(u.path[i], sizeof(u.path[i]), "%s/demo_rpc_upload_%d.txt", dir, i);
    u.fd[i] = open(u.path[i], O_CREAT | O_TRUNC | O_RDWR, 0644);
    if (u.fd[i] < 0 || pwrite(u.fd[i], text, UPLOAD_BYTES, 0) != UPLOAD_BYTES ||
        fsync(u.fd[i]) != 0) {
      perror(u.path[i]);
      exit(1);
    }
    posix_fadvise(u.fd[i], 0, UPLOAD_BYTES, POSIX_FADV_DONTNEED);
    u.buf[i] = (char*)calloc(1, UPLOAD_BYTES);
  }
  filc_async_get_stats(&u.start);
  fasync_get_stats(&u.io_start);
  calls.t0 = now_ms();
  return u;
}

// Waits for every upload and checks the server's checksums.
static inline int upload_report(struct upload* u, void** sent) {
  long reply[UPLOAD_FILES];
  for (int i = 0; i < UPLOAD_FILES; i++)
    reply[i] = wait_for(sent[i]);
  double total_ms = now_ms() - calls.t0;
  double loop_ms = calls.n ? calls.ms[(calls.n < LOG_MAX ? calls.n : LOG_MAX) - 1] : 0;
  filc_async_stats s;
  struct fasync_stats io;
  filc_async_get_stats(&s);
  fasync_get_stats(&io);
  unsigned long reads = io.sqes_queued - u->io_start.sqes_queued;
  unsigned long submits = io.kernel_submit_entries - u->io_start.kernel_submit_entries;

  int matched = 0;
  printf("  %-6s %-6s %-18s %s\n", "file", "bytes", "server checksum",
         "file checksum");
  for (int i = 0; i < UPLOAD_FILES; i++) {
    uint32_t mine = upload_checksum(u->buf[i], UPLOAD_BYTES);
    printf("  %-6d %-6d 0x%08lx         0x%08x\n", i, UPLOAD_BYTES,
           (unsigned long)reply[i], u->expect[i]);
    matched += reply[i] == (long)u->expect[i] && mine == u->expect[i];
  }
  printf("\n  the loop's last call ran at %.2f ms; the last upload was answered "
         "after %.1f ms\n",
         loop_ms, total_ms);
  printf("  the %lu reads reached the kernel in %lu submit\n", reads, submits);
  printf("\n  => each upload waited for its own read, and the reads still went out"
         " as one batch\n");

  unsigned long made = s.tasks_submitted - u->start.tasks_submitted;
  unsigned long completed = s.tasks_completed - u->start.tasks_completed;
  unsigned long failed = s.tasks_failed - u->start.tasks_failed;
  check("every upload sent the bytes its read produced", matched == UPLOAD_FILES);
  check("the reads reached the kernel in one submit",
        reads == UPLOAD_FILES && submits == 1);
  check("every call completed without failing",
        made == 2 * UPLOAD_FILES && completed == 2 * UPLOAD_FILES && failed == 0);

  for (int i = 0; i < UPLOAD_FILES; i++) {
    close(u->fd[i]);
    unlink(u->path[i]);
    free(u->buf[i]);
  }
  return finish();
}
