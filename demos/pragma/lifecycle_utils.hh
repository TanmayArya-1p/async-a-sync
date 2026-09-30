#pragma once

// Setup and report for demo_pragma_lifecycle.

#include "pragma_utils.hh"

struct demo {
  char path[256];
  const char* text;
  size_t len;
  double t0;
  struct counters start;
};

static inline struct demo setup(int argc, char** argv, const char* text) {
  struct demo d = {0};
  title("lifecycle: open, write, fsync, read, close",
        "  Five annotated calls, issued with no wait between them.");
  snprintf(d.path, sizeof(d.path), "%s/demo_pragma_lifecycle.txt",
           dir_arg(argc, argv));
  unlink(d.path);
  d.text = text;
  d.len = strlen(text);
  d.start = counters_now();
  d.t0 = now_ms();
  return d;
}

// Waits for the four calls after the open, then checks the file.
static inline int report(struct demo* d, int fd, const char* back, void* wrote,
                         void* synced, void* read_back, void* closed) {
  long wrote_n = wait_for(wrote);
  long synced_n = wait_for(synced);
  long read_n = wait_for(read_back);
  long closed_n = wait_for(closed);
  double ms = now_ms() - d->t0;
  struct counters used = counters_since(d->start);

  // The log holds the five calls in the order they ran.
  char result[5][96];
  snprintf(result[0], sizeof(result[0]), "fd %d", fd);
  snprintf(result[1], sizeof(result[1]), "%ld bytes written", wrote_n);
  snprintf(result[2], sizeof(result[2]), "%ld", synced_n);
  snprintf(result[3], sizeof(result[3]), "%ld bytes: \"%s\"", read_n, back);
  snprintf(result[4], sizeof(result[4]), "%ld", closed_n);
  printf("  %-14s %8s   %s\n", "call", "ran at", "result");
  for (int i = 0; i < calls.n && i < 5; i++)
    printf("  %-14s %5.2f ms   %s\n", calls.call[i].name,
           calls.call[i].ms - d->t0, result[i]);
  printf("\n  %lu calls, %lu io_uring requests, %lu kernel entries, %.2f ms\n",
         used.calls, used.sqes, used.submits + used.waits, ms);
  printf("\n  => 5 annotated syscalls ran in program order, no wait between "
         "them\n");

  // Check the file with ordinary blocking calls.
  char disk[128] = {0};
  int check_fd = open(d->path, O_RDONLY);
  ssize_t disk_n = check_fd >= 0 ? read(check_fd, disk, sizeof(disk)) : -1;
  if (check_fd >= 0)
    close(check_fd);
  errno = 0;
  int fd_closed = fcntl(fd, F_GETFD) == -1 && errno == EBADF;

  check("every call succeeded",
        fd >= 0 && wrote_n == (long)d->len && synced_n == 0 &&
            read_n == (long)d->len && closed_n == 0);
  check("the read saw the write issued before it", equal(back, d->text, d->len));
  check("the file holds the text (checked with plain read)",
        disk_n == (ssize_t)d->len && equal(disk, d->text, d->len));
  check("the fd was closed by the last call", fd_closed);
  check("five calls, five requests, five bodies run",
        used.calls == 5 && used.sqes == 5 && calls.n == 5);

  unlink(d->path);
  return finish();
}
