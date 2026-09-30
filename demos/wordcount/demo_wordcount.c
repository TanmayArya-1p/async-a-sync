/* wordcount: one loop, built blocking or implicit.
 *
 * Built plainly, it reads each file with pread. Built with
 * -DFASYNC_IMPLICIT and the patched compiler, it queues each read with
 * fasync_pread instead, and counting a buffer waits for its read. */

#include "wordcount_utils.hh"

int main(int argc, char** argv) {
  setup(argc, argv);

  for (int i = 0; i < files.n; i++)
#ifdef FASYNC_IMPLICIT
    fasync_pread(files.fd[i], files.buf[i], files.bytes, 0); // queues the read
#else
    pread(files.fd[i], files.buf[i], files.bytes, 0);
#endif

  size_t words = 0;
  for (int i = 0; i < files.n; i++)
    words += count_words(files.buf[i]); // implicit: waits for that file

  return report(words);
}
