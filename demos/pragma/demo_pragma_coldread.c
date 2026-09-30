/* coldread: the same loop, blocking and annotated.
 *
 * Counts the words in 512 files. Each pass starts from a dropped page cache,
 * so every read goes to the device.
 *
 *   A  read_blocking   pread in a loop, then count
 *   B  read_annotated  the same loop, calling async_pread
 *   C  read_by_hand    the same work with the explicit fasync API
 *
 * Usage: demo_pragma_coldread [dir] [files] [passes] */

#include "pragma_utils.hh"

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

int main(int argc, char** argv) {
  struct timing t = coldread_setup(argc, argv);

  for (int pass = 0; pass < t.passes; pass++) {
    time_pass(&t.blocking[pass], read_blocking, t.files);
    time_pass(&t.annotated[pass], read_annotated, t.files);
    time_pass(&t.by_hand[pass], read_by_hand, t.files);
  }

  return coldread_report(&t);
}
