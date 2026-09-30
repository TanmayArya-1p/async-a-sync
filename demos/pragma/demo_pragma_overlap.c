/* overlap: the reads run while the program hashes.
 *
 * Reads 256 files and hashes each one, two ways:
 *
 *   A  read_then_hash   read a file, hash it, read the next.
 *                       The program sleeps through every read.
 *   B  async_then_hash  queue every read, then hash the files.
 *                       Each file has landed by the time its hash starts.
 *
 * The report also times the reads and the hashing alone: A takes about
 * both, B about the hashing alone.
 *
 * Usage: demo_pragma_overlap [dir] [files] [passes] [rounds] */

#include "overlap_utils.hh"

// pread: the kernel fills buf.
#pragma clang attribute push(__attribute__((annotate("filc_async", "runtime=io_uring", "op=pread", "bout=buf"))), apply_to=function)
void* async_pread(int fd, void* buf, size_t len, unsigned long offset) {
  log_call("async_pread");
  return 0;
}
#pragma clang attribute pop

// A: read, hash, read, hash.
static unsigned long read_then_hash(int n) {
  unsigned long h = 0;
  for (int i = 0; i < n; i++) {
    pread(files.fd[i], files.buf[i], files.bytes, 0);
    h ^= hash_file(files.buf[i]);
  }
  return h;
}

// B: queue every read, then hash.
static unsigned long async_then_hash(int n) {
  for (int i = 0; i < n; i++)
    async_pread(files.fd[i], files.buf[i], files.bytes, 0);
  unsigned long h = 0;
  for (int i = 0; i < n; i++)
    h ^= hash_file(files.buf[i]); // waits for that file, if needed
  return h;
}

int main(int argc, char** argv) {
  struct demo d = setup(argc, argv);

  for (int pass = 0; pass < d.passes; pass++) {
    time_run(&d.blocking[pass], read_then_hash);
    time_run(&d.async[pass], async_then_hash);
  }

  return report(&d);
}
