/* demo_async_io run file: seed one pattern file, open it once, run the three
 * scenarios, and report. */

#include <string.h>

#include "demo_async_io.hh"

#define N_BLOCKS 64
#define BLOCK_SIZE 262144 /* 256 KiB */

int main(int argc, char** argv) {
  const char* dir = argc > 1 ? argv[1] : "/tmp";

  char path[256];
  snprintf(path, sizeof(path), "%s/async-a-sync_demo_payload.bin", dir);
  int loaded = demo_seed_file(path, N_BLOCKS, BLOCK_SIZE) == 0;
  int fd = loaded ? open(path, O_RDONLY) : -1;
  if (fd < 0)
    return 1;

  int rc = 0;
  rc |= scenario_lazy(fd, N_BLOCKS, BLOCK_SIZE);
  rc |= scenario_throughput(fd, N_BLOCKS, BLOCK_SIZE);
  rc |= scenario_dependencies();

  close(fd);
  unlink(path);
  printf("\n%s\n", rc ? "DEMO FAILED" : "DEMO OK");
  return rc;
}