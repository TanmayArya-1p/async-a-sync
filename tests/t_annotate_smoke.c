/* tests/t_annotate_smoke.c -- compile with the PATCHED clang, dump IR, grep
 * for the global.annotations entries. Not a runtime test. */
#pragma clang attribute push(__attribute__((annotate("filc_async", "runtime=io_uring", "op=pread", "fd=0", "buf=1"))), apply_to=function)
void* procread(int fd, void* buf, unsigned long n);
#pragma clang attribute pop

int main(void) { void* p = procread(0, 0, 0); return p != 0; }