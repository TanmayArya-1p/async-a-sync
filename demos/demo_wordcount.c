#include <stdio.h>

#include "demo_wordcount.hh"

#define FILES 512

#ifndef FASYNC_IMPLICIT
#define DEMO_LABEL "sync"
#else
#define DEMO_LABEL "implicit"
#endif

int main(int argc, char** argv) {
  const char* dir = argc > 1 ? argv[1] : "/tmp";

  if (demo_files(dir, FILES, 4096) < 0)
    return 1;

  size_t expected = 0;
  for (int i = 0; i < FILES; i++)
    expected += demo_expect[i];

  demo_cold();
  demo_start();

  size_t words = demo_wordcount();

  double ms = demo_elapsed();
  printf("  %-9s %7.2f ms  (%zu words)\n", DEMO_LABEL, ms, words);

  demo_finish();
  return words != expected;
}