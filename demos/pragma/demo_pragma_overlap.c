/* demo_pragma_overlap: the reads run while the program hashes.
 *
 * Reads 256 files and hashes each one, two ways:
 *
 *   A  read_then_hash   pread a file, hash it, pread the next: the program
 *                       sleeps through every read before it can hash
 *   B  async_then_hash  async_pread every file, then hash them: the reads
 *                       are in flight together and each has landed by the
 *                       time the hash loop reaches its file
 *
 * Each half is also timed alone, so A reads as reads + hashing and B as about
 * the hashing alone. pragma_report.hh sizes the hash to take as long as the
 * blocking reads, runs everything from a dropped page cache and prints the
 * medians.
 *
 * Usage: demo_pragma_overlap [dir] [files] [passes] [rounds] */

#include "pragma_report.hh"

/* A polynomial hash over 8-byte words, mixing each one hash_rounds times so
 * the hash costs compute, not memory access. Every step stays below 2^35, so
 * it never overflows. */
long hash_file(const void* p) {
  const long* w = (const long*)p;
  long h = 0;
  for (size_t i = 0; i < demo_bytes / sizeof(*w); i++)
    for (int r = 0; r < hash_rounds; r++)
      h = (h * 31 + w[i] % 1000003 + r) % 1000000007;
  return h;
}

long reads_only(int n) {
  for (int i = 0; i < n; i++)
    pread(demo_fd[i], demo_buf[i], demo_bytes, 0);
  return 0;
}

long async_reads_only(int n) {
  for (int i = 0; i < n; i++)
    async_pread(demo_fd[i], demo_buf[i], demo_bytes, 0);
  long first_bytes = 0;
  for (int i = 0; i < n; i++)
    first_bytes += demo_buf[i][0]; /* waits for that file's read */
  return first_bytes;
}

long hash_only(int n) {
  long h = 0;
  for (int i = 0; i < n; i++)
    h ^= hash_file(demo_buf[i]);
  return h;
}

long read_then_hash(int n) {
  long h = 0;
  for (int i = 0; i < n; i++) {
    pread(demo_fd[i], demo_buf[i], demo_bytes, 0);
    h ^= hash_file(demo_buf[i]);
  }
  return h;
}

long async_then_hash(int n) {
  for (int i = 0; i < n; i++)
    async_pread(demo_fd[i], demo_buf[i], demo_bytes, 0);
  long h = 0;
  for (int i = 0; i < n; i++)
    h ^= hash_file(demo_buf[i]);
  return h;
}

int main(int argc, char** argv) {
  struct overlap o = overlap_setup(argc, argv);
  overlap_calibrate(&o, reads_only, hash_only);

  for (int pass = 0; pass < o.passes; pass++) {
    time_run(&o, pass, READS_ONLY, reads_only);
    time_run(&o, pass, ASYNC_READS_ONLY, async_reads_only);
    time_run(&o, pass, HASH_ONLY, hash_only);
    time_run(&o, pass, READ_THEN_HASH, read_then_hash);
    time_run(&o, pass, ASYNC_THEN_HASH, async_then_hash);
  }

  return overlap_report(&o);
}
