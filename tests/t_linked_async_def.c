#include <stddef.h>

volatile int linked_body_calls;

__attribute__((noinline)) void* linked_pread(int fd, void* buf, size_t len,
                                             unsigned long offset)
{
    linked_body_calls++;
    return (void*)(long)(fd + (buf != NULL) + len + offset);
}