/*
 * utils.hh -- the dull shared machinery behind the demos.
 *
 * A clock, a word list, a corpus generator, and the word counter the demos exist
 * to show being handed a buffer that is still being filled. None of it knows
 * anything about io_uring, which is the point: this is what the demos' own code
 * looks like too.
 *
 * Header-only and static, so each demo gets its own copy and there is nothing to
 * build or link separately. The unused attributes keep whichever helper a given
 * demo does not need from warning about itself.
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

/* Milliseconds on a monotonic clock. */
static inline double demo_now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

/* A deterministic generator, so every run builds the same corpus. */
static inline unsigned int demo_rand(unsigned int* state) {
  *state = *state * 1103515245u + 12345u;
  return *state >> 16;
}

/*
 * Fill a buffer with words separated by spaces and newlines, and return how many
 * words went in, so a caller can check a count without trusting it.
 */
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
 * The ordinary word counter.
 *
 * noinline on purpose: inlined into main it would sit next to the submission, and
 * the demos would stop showing what they are about -- an ordinary callee being
 * handed a buffer the kernel has not finished filling.
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

/*
 * Write `files` files of `bytes` each under `dir`, named <prefix>_0000.txt, and
 * record their paths. Each one is fsynced, so the reads the demos then do are real
 * device reads rather than hits on the writer's dirty cache.
 *
 * `words_out`, if given, gets the number of words each file was built from -- the
 * count a reader should arrive at, known without having to trust the reader.
 */
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

/* Drop these files from the page cache, so the next read costs a device trip. */
static inline void demo_drop_caches(int* fd, int files, size_t bytes) {
  for (int i = 0; i < files; i++)
    posix_fadvise(fd[i], 0, bytes, POSIX_FADV_DONTNEED);
}

static inline void demo_remove_corpus(char paths[][DEMO_PATH_MAX], int files) {
  for (int i = 0; i < files; i++)
    unlink(paths[i]);
}

/*
 * One blocking read pass over every file, timed. Used by a demo to find out
 * whether this filesystem has any device latency to overlap at all -- on tmpfs it
 * has none, and a comparison between the two ways of reading is meaningless.
 */
static inline double demo_read_pass(int* fd, unsigned char** buf, int files,
                                    size_t bytes) {
  double t = demo_now_ms();
  for (int i = 0; i < files; i++)
    if (pread(fd[i], buf[i], bytes, 0) != (ssize_t)bytes)
      break;
  return demo_now_ms() - t;
}

#endif /* DEMO_UTILS_HH */
