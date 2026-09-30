/* plain_io: ask for every file, then count every file.
 *
 * fasync_pread only queues a read. Counting a buffer waits for its read.
 * No submit, no wait. */

#include "explicit_utils.hh"

int main(int argc, char** argv) {
  plain_io_setup(argc, argv);

  for (int i = 0; i < files.n; i++)
    fasync_pread(files.fd[i], files.buf[i], files.bytes, 0); // queues the read

  int correct = 0;
  for (int i = 0; i < files.n; i++)
    correct += count_words(files.buf[i]) == files.words[i]; // waits for that file

  return plain_io_report(correct);
}
