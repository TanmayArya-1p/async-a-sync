/* demo_provenance run file: seed one file, open it twice, run the crux, report. */

#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

#include "demo_provenance.hh"

#define N_BLOCKS 2
#define BLOCK_SIZE 65536

int main(int argc, char** argv) {
  const char* dir = argc > 1 ? argv[1] : "/tmp";

  char path[256];
  snprintf(path, sizeof(path), "%s/async-a-sync_provenance_payload.bin", dir);
  if (demo_seed_file(path, N_BLOCKS, BLOCK_SIZE) != 0)
    return 1;
  int fdw = open(path, O_WRONLY);
  int fdr = open(path, O_RDONLY);
  if (fdw < 0 || fdr < 0)
    return 1;

  size_t len = (size_t)N_BLOCKS * BLOCK_SIZE;
  printf("  provenance: %zu bytes, on %s\n", len, dir);

  if (demo_prov_setup(len) < 0)
    return 1;
  int verified = demo_provenance(fdw, fdr);
  printf("  %d/2 claims verified\n", verified);
  demo_prov_teardown();

  close(fdw);
  close(fdr);
  unlink(path);
  return verified == 2 ? 0 : 1;
}