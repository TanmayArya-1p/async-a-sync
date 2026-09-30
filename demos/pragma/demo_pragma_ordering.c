/* demo_pragma_ordering: what the r_dep= and w_dep= options buy.
 *
 * In pragma_io.hh async_pwrite has `w_dep=fd:file` (it writes its fd) and
 * async_pread has `r_dep=fd:file` (it reads its fd). Two calls on the same key
 * conflict unless both only read it, and a conflicting call waits, inside the
 * call, for the one before it. Everything else only queues, and the whole
 * queue goes to the kernel in one submit the first time a result is needed.
 *
 * pragma_report.hh counts the kernel entries of each scenario and prints them
 * as a table. */

#include "pragma_report.hh"

#define FILES ORDERING_FILES
#define RECORD ORDERING_RECORD

/* Two writes and a read of the same bytes, all issued before any wait. */
static void same_file(int fd) {
  char buf[8] = {0};

  scenario_start("write, write, read, one file", CONFLICTING);
  void* first = async_pwrite(fd, "first", 5, 0);
  void* second = async_pwrite(fd, "last!", 5, 0);
  void* read_back = async_pread(fd, buf, 5, 0);
  scenario_issued();

  /* Waited on in reverse order: the read still sees the last write. */
  pragma_wait(read_back);
  pragma_wait(second);
  pragma_wait(first);
  scenario_done(memcmp(buf, "last!", 5) == 0, "read saw \"last!\"");
}

/* One write to each file: no two calls share a key. */
static void many_files(const int* fd) {
  static const char msg[] = "independent";
  void* task[FILES];

  scenario_start("8 writes to 8 different files", INDEPENDENT);
  for (int i = 0; i < FILES; i++)
    task[i] = async_pwrite(fd[i], msg, sizeof(msg) - 1, 0);
  scenario_issued();

  int landed = 0;
  for (int i = 0; i < FILES; i++)
    landed += pragma_wait(task[i]) == (long)(sizeof(msg) - 1);
  scenario_done(landed == FILES, "all 8 landed");
}

/* Eight reads of one file. Reads of one fd do not conflict. Each read gets
 * its own buffer, because a pending mark covers a whole object. */
static void many_reads(int fd) {
  char* buf[FILES];
  for (int i = 0; i < FILES; i++)
    buf[i] = calloc(1, RECORD + 1);
  write_records(fd);

  scenario_start("8 reads of one file", INDEPENDENT);
  for (int i = 0; i < FILES; i++)
    async_pread(fd, buf[i], RECORD, (unsigned long)i * RECORD);
  scenario_issued();

  /* No handles and no waits: reading a buffer is enough. */
  int ok = 1;
  for (int i = 0; i < FILES; i++)
    ok &= holds_record(buf[i], i);
  scenario_done(ok, "all 8 correct");

  for (int i = 0; i < FILES; i++)
    free(buf[i]);
}

int main(int argc, char** argv) {
  int fd[FILES];
  ordering_setup(argc, argv, fd);

  same_file(fd[0]);
  many_files(fd);
  many_reads(fd[1]);

  return ordering_report(fd);
}
