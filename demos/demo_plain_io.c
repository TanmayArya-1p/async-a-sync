#include <stdio.h>

#include "demo_plain_io.hh"

#define FILES 4
#define BYTES (512 * 1024)

int main(int argc, char** argv) {
  const char* dir = argc > 1 ? argv[1] : "/tmp";

  if (demo_files(dir, FILES, BYTES) < 0)
    return 1;
  demo_cold();

  printf("  plain_io: %d files, %d KiB each, on %s\n", FILES, BYTES / 1024, dir);
  int correct = demo_plain_io();
  demo_show_submit("result");
  printf("  %d/%d files correct\n", correct, FILES);

  demo_finish();
  return correct == FILES ? 0 : 1;
}
