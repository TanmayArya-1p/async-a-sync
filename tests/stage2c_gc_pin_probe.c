/*
 * stage2c_gc_pin_probe.c -- can GC memory hold a pointer the kernel keeps?
 *
 * IORING_SETUP_NO_MMAP would let us hand the kernel a buffer we allocated, which
 * is attractive because then the ring lives in ordinary Fil-C memory and the
 * memory-safe half can read the completion queue directly, with no copying and
 * no extra trips across the trusted boundary.
 *
 * That only works if the allocation never moves. Fil-C's runtime exposes
 * zscavenge_synchronously() and zgc_request_fresh(), so the collector does
 * scavenge; the question is whether the large-object path relocates.
 *
 * This probe allocates a page-aligned buffer, records its address, forces
 * several collection cycles, and checks whether the address and contents
 * survived. If the address is stable, IORING_SETUP_NO_MMAP is safe to build on.
 *
 * Build:
 *   filcc -O2 -static -o stage2c_gc_pin_probe tests/stage2c_gc_pin_probe.c
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include <stdfil.h>

#define RING_BYTES (2u * 1024u * 1024u)

int main(void) {
  void* rings = zgc_aligned_alloc(4096, RING_BYTES);
  void* sqes = zgc_aligned_alloc(4096, 64 * 1024);

  uintptr_t before_rings = (uintptr_t)rings;
  uintptr_t before_sqes = (uintptr_t)sqes;

  printf("rings=%p sqes=%p\n", rings, sqes);
  printf("page-aligned: rings=%d sqes=%d\n",
         (before_rings & 4095u) == 0, (before_sqes & 4095u) == 0);

  unsigned char* r = (unsigned char*)rings;
  unsigned char* s = (unsigned char*)sqes;
  for (unsigned i = 0; i < 4096; i++) {
    r[i] = (unsigned char)(0xA0 + (i & 15));
    s[i] = (unsigned char)(0x50 + (i & 15));
  }

  /* Force real collection work. Allocate a pile of garbage first so there is
   * something to scavenge. */
  for (unsigned i = 0; i < 20000; i++) {
    void* g = zgc_alloc(1024);
    memset(g, 1, 1024);
  }

  unsigned long long cycle = zgc_request_fresh();
  zgc_wait(cycle);
  zscavenge_synchronously();
  zscavenge_synchronously();

  uintptr_t after_rings = (uintptr_t)rings;
  uintptr_t after_sqes = (uintptr_t)sqes;

  int contents_ok = 1;
  for (unsigned i = 0; i < 4096; i++) {
    if (r[i] != (unsigned char)(0xA0 + (i & 15))) contents_ok = 0;
    if (s[i] != (unsigned char)(0x50 + (i & 15))) contents_ok = 0;
  }

  printf("after %llu cycle + 2 scavenges:\n", cycle);
  printf("  rings moved: %s\n", before_rings == after_rings ? "no" : "YES");
  printf("  sqes  moved: %s\n", before_sqes == after_sqes ? "no" : "YES");
  printf("  contents intact: %s\n", contents_ok ? "yes" : "NO");

  int ok = (before_rings == after_rings) && (before_sqes == after_sqes) &&
           contents_ok && ((before_rings & 4095u) == 0) &&
           ((before_sqes & 4095u) == 0);
  printf("\nSTAGE2C %s\n", ok ? "PASS" : "FAIL");
  return ok ? 0 : 1;
}
