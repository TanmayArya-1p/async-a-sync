#pragma once

// Buffers, checks, setup and report for demo_provenance.

#include "explicit_utils.hh"

// Both calls' handles.
struct pair {
  fasync_id write, read;
};

struct demo {
  char path[256];
  int fd_write, fd_read; // two fds on one file
  size_t len;
  unsigned char* written;        // what the write puts in the file
  unsigned char* read_untracked; // the read without a tracker
  unsigned char* read_tracked;   // the read with one
};

// Puts other bytes in the file.
static inline void reset_file(struct demo* d) {
  unsigned char* old_bytes = (unsigned char*)malloc(d->len);
  memset(old_bytes, 0xA7, d->len);
  if (pwrite(d->fd_write, old_bytes, d->len, 0) != (ssize_t)d->len) {
    perror(d->path);
    exit(1);
  }
  free(old_bytes);
}

static inline struct demo setup(int argc, char** argv) {
  struct demo d = {0};
  title("provenance: a write and a read through two fds",
        "  Nothing shows they touch the same bytes. A tracker orders them.");
  d.len = 128 * 1024;
  d.written = (unsigned char*)malloc(d.len);
  d.read_untracked = (unsigned char*)calloc(1, d.len);
  d.read_tracked = (unsigned char*)calloc(1, d.len);
  memset(d.written, 0x5E, d.len);
  snprintf(d.path, sizeof(d.path), "%s/demo_provenance.bin", dir_arg(argc, argv));
  d.fd_write = open(d.path, O_CREAT | O_TRUNC | O_WRONLY, 0644);
  d.fd_read = open(d.path, O_RDONLY);
  if (d.fd_write < 0 || d.fd_read < 0) {
    perror(d.path);
    exit(1);
  }
  reset_file(&d);
  return d;
}

// Without a tracker both calls are queued and neither has run.
static inline void check_untracked(struct demo* d, struct pair c) {
  int in_flight = !fasync_ready(c.write) && !fasync_ready(c.read);
  int done = fasync_result(c.write) == (long)d->len &&
             fasync_result(c.read) == (long)d->len;
  printf("  no tracker:  both calls in flight at once: %s\n",
         in_flight ? "yes" : "no");
  check("without a tracker, nothing ordered the two calls", in_flight && done);
  reset_file(d);
}

// With a tracker the read waited for the write, and saw its bytes.
static inline void check_tracked(struct demo* d, struct pair c) {
  int ordered = fasync_ready(c.write);
  int done = fasync_result(c.write) == (long)d->len &&
             fasync_result(c.read) == (long)d->len;
  int saw_write = equal(d->read_tracked, d->written, d->len);
  printf("  one tracker: the write finished before the read: %s; "
         "the read saw it: %s\n",
         ordered ? "yes" : "no", saw_write ? "yes" : "no");
  check("with a tracker, the read ran after the write", ordered && done);
  check("and saw the written bytes", saw_write);
}

static inline int report(struct demo* d) {
  printf("\n");
  close(d->fd_write);
  close(d->fd_read);
  unlink(d->path);
  free(d->written);
  free(d->read_untracked);
  free(d->read_tracked);
  return finish();
}
