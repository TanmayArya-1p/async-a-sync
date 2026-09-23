/* tests/t_annotate_smoke.c -- compile with the PATCHED clang, dump IR, grep
 * for the global.annotations entries. Not a runtime test. */
#pragma clang attribute push(__attribute__((annotate("filc_async", "op=pread", "fd=0", "buf=1"))), apply_to=function)
int procread(int fd, void* buf, unsigned long n);
#pragma clang attribute pop

int main(void) { return procread(0, 0, 0); }