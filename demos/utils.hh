#pragma once

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifdef FASYNC_IMPLICIT
#include "fasync.h"
#ifndef FASYNC_COMPILER_INSERTS_CHECKS
#error "the implicit backend needs the patched compiler: \
build with -DFASYNC_COMPILER_INSERTS_CHECKS"
#endif
#endif

#define DEMO_PATH_MAX 128
#define DEMO_MAX_FILES 2048

static char demo_paths[DEMO_MAX_FILES][DEMO_PATH_MAX];
static int demo_fd[DEMO_MAX_FILES];
static unsigned char* demo_buf[DEMO_MAX_FILES];
static size_t demo_expect[DEMO_MAX_FILES];
static int demo_n;
static size_t demo_bytes;

static inline double demo_now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

static double demo_t0;

static inline void demo_start(void) {
  demo_t0 = demo_now_ms();
}

static inline double demo_elapsed(void) {
  return demo_now_ms() - demo_t0;
}

static inline unsigned int demo_rand(unsigned int* state) {
  *state = *state * 1103515245u + 12345u;
  return *state >> 16;
}

static const char* const demo_words[] = {
    "alpha", "beta", "gamma", "delta", "epsilon"};
#define DEMO_NWORDS ((int)(sizeof(demo_words) / sizeof(demo_words[0])))

static inline size_t demo_fill_words(unsigned char* p, size_t cap,
                                     unsigned int* state) {
  size_t n = 0;
  size_t words = 0;
  while (n < cap) {
    const char* w = demo_words[demo_rand(state) % DEMO_NWORDS];
    size_t wl = strlen(w);
    if (n + wl + (words ? 1 : 0) > cap)
      break;
    if (words)
      p[n++] = (demo_rand(state) & 8) ? '\n' : ' ';
    memcpy(p + n, w, wl);
    n += wl;
    words++;
  }
  memset(p + n, ' ', cap - n);
  return words;
}

__attribute__((noinline))
static size_t wordcount(const char* p) {
  size_t words = 0;
  int in_word = 0;
  for (size_t i = 0; i < demo_bytes; i++) {
    int separator = (p[i] == ' ' || p[i] == '\n');
    if (!separator && !in_word)
      words++;
    in_word = !separator;
  }
  return words;
}

static inline int demo_files(const char* dir, int n, size_t bytes) {
  if (n > DEMO_MAX_FILES)
    return -1;
  demo_n = n;
  demo_bytes = bytes;

  unsigned int state = 7;
  unsigned char* b = (unsigned char*)malloc(bytes);
  if (!b)
    return -1;

  for (int i = 0; i < n; i++) {
    demo_expect[i] = demo_fill_words(b, bytes, &state);
    snprintf(demo_paths[i], DEMO_PATH_MAX, "%s/demo_%04d.txt", dir, i);
    int w = open(demo_paths[i], O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (w < 0 || pwrite(w, b, bytes, 0) != (ssize_t)bytes) {
      printf("cannot write %s\n", demo_paths[i]);
      free(b);
      return -1;
    }
    fsync(w);
    close(w);

    demo_buf[i] = (unsigned char*)malloc(bytes);
    memset(demo_buf[i], 0, bytes);
    demo_fd[i] = open(demo_paths[i], O_RDONLY);
    if (demo_fd[i] < 0) {
      free(b);
      return -1;
    }
  }
  free(b);
  return 0;
}

static inline void demo_cold(void) {
  for (int i = 0; i < demo_n; i++)
    posix_fadvise(demo_fd[i], 0, demo_bytes, POSIX_FADV_DONTNEED);
}

static inline const char* read_file(int i) {
  if (pread(demo_fd[i], demo_buf[i], demo_bytes, 0) != (ssize_t)demo_bytes)
    return 0;
  return (const char*)demo_buf[i];
}

static inline void read_all_files(void) {
  for (int i = 0; i < demo_n; i++) {
#ifdef FASYNC_IMPLICIT
    memset(demo_buf[i], 0, demo_bytes);
    if (!fasync_pread(demo_fd[i], demo_buf[i], demo_bytes, 0))
      exit(1);
#else
    if (pread(demo_fd[i], demo_buf[i], demo_bytes, 0) != (ssize_t)demo_bytes)
      exit(1);
#endif
  }
}

static inline const char* file_data(int i) {
  return (const char*)demo_buf[i];
}

/* Close and delete the files, free the buffers. */
static inline void demo_finish(void) {
  for (int i = 0; i < demo_n; i++) {
    close(demo_fd[i]);
    unlink(demo_paths[i]);
    free(demo_buf[i]);
  }
}

static inline int demo_seed_file(const char* path, int blocks, size_t block_size) {
  unsigned char* block = (unsigned char*)malloc(block_size);
  if (!block)
    return -1;

  int fd = open(path, O_CREAT | O_TRUNC | O_RDWR, 0644);
  if (fd < 0) {
    free(block);
    return -1;
  }

  for (int i = 0; i < blocks; i++) {
    memset(block, (i + 1) & 0xFF, block_size);
    if (pwrite(fd, block, block_size, (off_t)i * block_size) != (ssize_t)block_size) {
      close(fd);
      free(block);
      return -1;
    }
  }
  close(fd);
  free(block);
  return 0;
}

static inline int demo_block_ok(const unsigned char* buf, size_t block_size,
                                int index) {
  unsigned char expected = (unsigned char)((index + 1) & 0xFF);
  for (size_t i = 0; i < block_size; i += 512)
    if (buf[i] != expected)
      return 0;
  return 1;
}


static unsigned char* demo_prov_author;       /* what the author writes       */
static unsigned char* demo_prov_plain_reader; /* an untagged read lands here  */
static unsigned char* demo_prov_tagged_reader; /* a tagged read lands here     */
static unsigned char* demo_prov_stale;        /* the "on disk" old bytes      */
static size_t demo_prov_len;

static inline int demo_prov_setup(size_t len) {
  demo_prov_author = (unsigned char*)malloc(len);
  demo_prov_plain_reader = (unsigned char*)malloc(len);
  demo_prov_tagged_reader = (unsigned char*)malloc(len);
  demo_prov_stale = (unsigned char*)malloc(len);
  if (!demo_prov_author || !demo_prov_plain_reader ||
      !demo_prov_tagged_reader || !demo_prov_stale)
    return -1;
  memset(demo_prov_author, 0x5E, len);
  memset(demo_prov_plain_reader, 0, len);
  memset(demo_prov_tagged_reader, 0, len);
  memset(demo_prov_stale, 0xA7, len);
  demo_prov_len = len;
  return 0;
}

static inline int demo_prov_resterile(int fd) {
  return pwrite(fd, demo_prov_stale, demo_prov_len, 0) == (ssize_t)demo_prov_len
             ? 0
             : -1;
}

static inline void demo_prov_teardown(void) {
  free(demo_prov_author);
  free(demo_prov_plain_reader);
  free(demo_prov_tagged_reader);
  free(demo_prov_stale);
}

#ifdef FASYNC_IMPLICIT

static inline void demo_show_submit(const char* note) {
  struct fasync_stats s;
  fasync_get_stats(&s);
  printf("  %s: %lu SQEs queued, %lu kernel entries, %lu blocking entries\n",
         note, s.sqes_queued, s.kernel_submit_entries, s.kernel_wait_entries);
}

#endif /* FASYNC_IMPLICIT */
