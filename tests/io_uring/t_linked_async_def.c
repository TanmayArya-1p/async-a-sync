/* The implementation behind t_linked_async_main.c's annotated declaration.
 * With LINKED_ANNOTATE_DEF it is annotated too, as it would be when both
 * files include one annotated header: the pass then renames this body, and
 * the caller's reference to linked_pread must still link. */
#include <stddef.h>

volatile int linked_body_calls;

#ifdef LINKED_ANNOTATE_DEF
#pragma clang attribute push(__attribute__((annotate("filc_async", "runtime=io_uring", "op=pread", "bout=buf"))), apply_to=function)
#endif
__attribute__((noinline)) void* linked_pread(int fd, void* buf, size_t len,
                                             unsigned long offset)
{
    linked_body_calls++;
    return (void*)(long)(fd + (buf != NULL) + len + offset);
}
#ifdef LINKED_ANNOTATE_DEF
#pragma clang attribute pop
#endif
