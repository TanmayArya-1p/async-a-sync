/* Compiler fixture: two dependency options on one argument must agree in
 * mode and namespace, or the pass rejects the function. check_dependency_
 * options.sh also derives an empty-name variant from this file. */
#pragma clang attribute push(__attribute__((annotate("filc_async", "runtime=io_uring", "op=fsync", "r_dep=fd:left", "r_dep=fd:right"))), apply_to=function)
void* conflicted(int fd) { return (void*)(long)fd; }
#pragma clang attribute pop

void* invoke(int fd)
{
    return conflicted(fd);
}
