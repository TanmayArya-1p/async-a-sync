/*
 * utils.hh -- the small shared helpers the demos use.
 * A clock, a word list, a corpus, and the word counter the demos are about.
 * Header-only, nothing to link.
 */
#ifndef DEMO_UTILS_HH
#define DEMO_UTILS_HH

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define DEMO_PATH_MAX 128

static const char* const demo_words[] __attribute__((unused)) = {
    "alpha", "beta", "gamma", "delta", "epsilon"};
#define DEMO_NWORDS ((int)(sizeof(demo_words) / sizeof(demo_words[0])))

/* Wall clock, in milliseconds. */
static inline double demo_now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

/* Deterministic random numbers, so every run makes the same corpus. */
static inline unsigned int demo_rand(unsigned int* state) {
  *state = *state * 1103515245u + 12345u;
  return *state >> 16;
}

/* Fill a buffer with words separated by spaces and newlines; return how many. */
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

/*
 * Counts words in p[0..n); a word is a run of non-space characters.
 *
 * noinline on purpose: the demos hand it a buffer the kernel is still filling,
 * which is only a real demo if this load stays in an ordinary function instead
 * of being inlined back into main.
 */
__attribute__((noinline))
static size_t demo_count_words(const char* p, size_t n) {
  size_t words = 0;
  int in_word = 0;
  for (size_t i = 0; i < n; i++) {
    int separator = (p[i] == ' ' || p[i] == '\n');
    if (!separator && !in_word)
      words++;
    in_word = !separator;
  }
  return words;
}

/* Make `files` files of `bytes` under dir; record their paths and word counts. */
static inline int demo_make_corpus(const char* dir, const char* prefix, int files,
                                   size_t bytes, char paths[][DEMO_PATH_MAX],
                                   size_t* words_out) {
  unsigned int state = 7;
  unsigned char* b = malloc(bytes);
  if (!b)
    return -1;

  for (int i = 0; i < files; i++) {
    size_t words = demo_fill_words(b, bytes, &state);
    if (words_out)
      words_out[i] = words;
    snprintf(paths[i], DEMO_PATH_MAX, "%s/%s_%04d.txt", dir, prefix, i);
    int fd = open(paths[i], O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (fd < 0 || pwrite(fd, b, bytes, 0) != (ssize_t)bytes) {
      printf("cannot write %s\n", paths[i]);
      free(b);
      return -1;
    }
    fsync(fd);
    close(fd);
  }
  free(b);
  return 0;
}

/* Drop these files from the page cache, so the next read hits the device. */
static inline void demo_drop_caches(int* fd, int files, size_t bytes) {
  for (int i = 0; i < files; i++)
    posix_fadvise(fd[i], 0, bytes, POSIX_FADV_DONTNEED);
}

/* Delete the corpus files. */
static inline void demo_remove_corpus(char paths[][DEMO_PATH_MAX], int files) {
  for (int i = 0; i < files; i++)
    unlink(paths[i]);
}

/* One blocking read pass, timed. Used to check this filesystem has device latency
 * to overlap at all (tmpfs has none). */
static inline double demo_read_pass(int* fd, unsigned char** buf, int files,
                                    size_t bytes) {
  double t = demo_now_ms();
  for (int i = 0; i < files; i++)
    if (pread(fd[i], buf[i], bytes, 0) != (ssize_t)bytes)
      break;
  return demo_now_ms() - t;
}

#endif /* DEMO_UTILS_HH */
