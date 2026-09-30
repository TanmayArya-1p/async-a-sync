/* scaling: how the gain changes with the number of reads.
 *
 * Runs coldread's three loops over the first N files, N from 1 to 2048.
 * One read has nothing to overlap with, so the three tie. As N grows, the
 * blocking loop pays N device round trips; the async loops pay about one
 * per batch. Above 1024 files the request table is full, so annotated calls
 * wait for room.
 *
 * Usage: demo_pragma_scaling [dir] [max files] [passes] */

#include "scaling_utils.hh"

// pread: the kernel fills buf.
#pragma clang attribute push(__attribute__((annotate("filc_async", "runtime=io_uring", "op=pread", "bout=buf"))), apply_to=function)
void* async_pread(int fd, void* buf, size_t len, unsigned long offset) {
  log_call("async_pread");
  return 0;
}
#pragma clang attribute pop

// A: one blocking read after another.
static size_t read_blocking(int n) {
  for (int i = 0; i < n; i++)
    pread(files.fd[i], files.buf[i], files.bytes, 0);
  size_t words = 0;
  for (int i = 0; i < n; i++)
    words += count_words(files.buf[i]);
  return words;
}

// B: the same loop. No handles, no waits.
static size_t read_annotated(int n) {
  for (int i = 0; i < n; i++)
    async_pread(files.fd[i], files.buf[i], files.bytes, 0);
  size_t words = 0;
  for (int i = 0; i < n; i++)
    words += count_words(files.buf[i]); // waits for that file
  return words;
}

// C: by hand, 512 reads at a time (the request table holds 1024).
static size_t read_by_hand(int n) {
  fasync_id id[512];
  size_t words = 0;
  for (int first = 0; first < n; first += 512) {
    int count = n - first < 512 ? n - first : 512;
    for (int i = 0; i < count; i++)
      id[i] = fasync_pread(files.fd[first + i], files.buf[first + i], files.bytes, 0);
    fasync_submit();
    for (int i = 0; i < count; i++) {
      fasync_result(id[i]);
      words += count_words(files.buf[first + i]);
    }
  }
  return words;
}

static const int sizes[] = {1, 4, 16, 64, 256, 512, 1024, 2048};

int main(int argc, char** argv) {
  struct demo d = setup(argc, argv);

  for (int s = 0; s < 8 && sizes[s] <= d.files; s++) {
    for (int pass = 0; pass < d.passes; pass++) {
      time_pass(&d.blocking[pass], read_blocking, sizes[s]);
      time_pass(&d.annotated[pass], read_annotated, sizes[s]);
      time_pass(&d.by_hand[pass], read_by_hand, sizes[s]);
    }
    row(&d, sizes[s]);
  }

  return report(&d);
}
