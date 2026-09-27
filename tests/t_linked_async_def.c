#include <stddef.h>

volatile int linked_body_calls;

#pragma clang attribute push(__attribute__((annotate("filc_async", "op=pread", "fd=0", "bout=1"))), apply_to=function)
__attribute__((noinline)) void* linked_pread(int fd, void* buf, size_t len,
                                             unsigned long offset)
{
    linked_body_calls++;
    return (void*)(long)(fd + (buf != NULL) + len + offset);
}
#pragma clang attribute pop
