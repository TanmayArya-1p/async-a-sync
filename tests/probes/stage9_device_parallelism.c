/* stage9_device_parallelism.c -- what this device can actually do in parallel.
 * Plain C, not a Fil-C test. It exists because stage8's implicit path beat
 * blocking by only ~2.3x on an uncached workload: if a handful of blocking
 * threads cannot beat the async path either, the ceiling is the device's; if
 * they go several times faster, the gap is the runtime's. Same workload as
 * stage8: 512 files of 4 KiB, page cache dropped per pass. */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>

#define N_FILES 512
#define FILE_BYTES 4096
#define MAX_THREADS 64

static char paths[N_FILES][256];
static int fd[N_FILES];

static double now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

struct worker_arg {
  int tid;
  int nthreads;
};

static void* worker(void* p) {
  struct worker_arg* a = (struct worker_arg*)p;
  unsigned char* buf = malloc(FILE_BYTES);
  for (int i = a->tid; i < N_FILES; i += a->nthreads)
    if (pread(fd[i], buf, FILE_BYTES, 0) != FILE_BYTES)
      break;
  free(buf);
  return 0;
}

int main(int argc, char** argv) {
  const char* dir = argc > 1 ? argv[1] : ".";

  for (int i = 0; i < N_FILES; i++) {
    snprintf(paths[i], sizeof(paths[i]), "%s/dev_%04d.bin", dir, i);
    int w = open(paths[i], O_CREAT | O_TRUNC | O_RDWR, 0644);
    char* b = malloc(FILE_BYTES);
    memset(b, 0x33, FILE_BYTES);
    if (w < 0 || pwrite(w, b, FILE_BYTES, 0) != FILE_BYTES) {
      printf("cannot create %s\n", paths[i]);
      return 1;
    }
    fsync(w);
    close(w);
    free(b);
    fd[i] = open(paths[i], O_RDONLY);
    if (fd[i] < 0)
      return 1;
  }

  int counts[] = {1, 2, 4, 8, 16, 32, 64};
  int n_counts = (int)(sizeof(counts) / sizeof(counts[0]));

  printf("%d files x %d bytes, page cache dropped before each pass\n", N_FILES,
         FILE_BYTES);
  printf("plain blocking reads, so this measures the device and not the runtime\n\n");
  printf("  %7s %10s %10s %10s\n", "threads", "ms", "us/file", "kIOPS");

  double best = 0;
  double best_us = 0;
  double serial_us = 0;

  for (int c = 0; c < n_counts; c++) {
    int nt = counts[c];
    for (int i = 0; i < N_FILES; i++)
      posix_fadvise(fd[i], 0, FILE_BYTES, POSIX_FADV_DONTNEED);

    pthread_t th[MAX_THREADS];
    struct worker_arg args[MAX_THREADS];
    double t0 = now_ms();
    for (int i = 0; i < nt; i++) {
      args[i].tid = i;
      args[i].nthreads = nt;
      pthread_create(&th[i], 0, worker, &args[i]);
    }
    for (int i = 0; i < nt; i++)
      pthread_join(th[i], 0);
    double ms = now_ms() - t0;

    double kbps = N_FILES / ms;
    printf("  %7d %10.2f %10.2f %10.1f\n", nt, ms, ms * 1000.0 / N_FILES, kbps);

    if (nt == 1)
      serial_us = ms * 1000.0 / N_FILES;
    if (kbps > best) {
      best = kbps;
      best_us = ms * 1000.0 / N_FILES;
    }
  }

  printf("\n  serial:                 %8.2f us/file\n", serial_us);
  printf("  best parallel:          %8.2f us/file  (%.1f kIOPS)\n", best_us, best);
  printf("  parallelism available:  %8.2fx\n", serial_us / (best_us > 0 ? best_us : 1e-9));

  printf("\n  Read this against stage8: its implicit arm ran the same workload at\n"
         "  about 19 us/file. Whatever this probe finds above that is concurrency\n"
         "  the kernel can sustain and the runtime is not currently reaching.\n");

  for (int i = 0; i < N_FILES; i++) {
    close(fd[i]);
    unlink(paths[i]);
  }

  printf("\nSTAGE9 %s\n", best > 0 ? "PASS" : "FAIL");
  return best > 0 ? 0 : 1;
}
