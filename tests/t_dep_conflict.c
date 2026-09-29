// Negative fixture: two dependencies on one argument whose mode or :<name>
// namespace differ must be a compile-time fatal.
#pragma clang attribute push(__attribute__((annotate("filc_async", "op=fsync", "fd=0", "r_dep=0:left", "w_dep=0:right"))), apply_to=function)
__attribute__((noinline)) void* conflicted(int fd)
{
    volatile int sink = fd;
    (void)sink;
    return 0;
}
#pragma clang attribute pop

int main(void)
{
    return 0;
}