/* ordering: what r_dep= and w_dep= buy.
 *
 * async_pwrite writes its fd (w_dep=fd:file); async_pread reads it
 * (r_dep=fd:file). Calls on the same fd conflict unless both only read.
 * A conflicting call waits, inside the call, for the one before it.
 * Other calls only queue, and go to the kernel together. */

#include "ordering_utils.hh"

#pragma clang attribute push(__attribute__((annotate("filc_async", "runtime=io_uring", "op=pwrite", "bin=buf", "w_dep=fd:file"))), apply_to=function)
void* async_pwrite(int fd, const void* buf, size_t len, unsigned long offset) {
  log_call("async_pwrite");
  return 0;
}
#pragma clang attribute pop

#pragma clang attribute push(__attribute__((annotate("filc_async", "runtime=io_uring", "op=pread", "bout=buf", "r_dep=fd:file"))), apply_to=function)
void* async_pread(int fd, void* buf, size_t len, unsigned long offset) {
  log_call("async_pread");
  return 0;
}
#pragma clang attribute pop

// Two writes and a read of the same bytes.
static int same_file(int fd) {
  char buf[8] = {0};
  async_pwrite(fd, "first", 5, 0);
  async_pwrite(fd, "last!", 5, 0); // waits for the first write
  async_pread(fd, buf, 5, 0);      // waits for both writes
  return equal(buf, "last!", 5);   // waits for the read
}

// One write to each file: no two calls conflict.
static int many_files(const int* fd) {
  void* task[FILES];
  for (int i = 0; i < FILES; i++)
    task[i] = async_pwrite(fd[i], "independent", 11, 0);
  int landed = 0;
  for (int i = 0; i < FILES; i++)
    landed += wait_for(task[i]) == 11;
  return landed == FILES;
}

// Eight reads of one file: readers do not conflict.
static int many_reads(int fd, char** record) {
  for (int i = 0; i < FILES; i++)
    async_pread(fd, record[i], RECORD, i * RECORD);
  int ok = 1;
  for (int i = 0; i < FILES; i++)
    ok &= is_record(record[i], i); // waits for that read
  return ok;
}

int main(int argc, char** argv) {
  struct demo d = setup(argc, argv);

  scenario("write, write, read, one file", same_file(d.fd[0]), WAITS);
  scenario("8 writes to 8 different files", many_files(d.fd), BATCHES);
  scenario("8 reads of one file", many_reads(d.records, d.record), BATCHES);

  return report(&d);
}
