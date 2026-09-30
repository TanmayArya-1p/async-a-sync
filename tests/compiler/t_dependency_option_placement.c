/* Compiler fixture: dependency options follow the same declaration rules as
 * op= and bout=, and each one names a parameter and a namespace. Every
 * function is called so Clang retains its annotation. */
#pragma clang attribute push(__attribute__((annotate("filc_async", "runtime=io_uring", "op=pread", "bout=buf", "r_dep=fd:file", "w_dep=buf:mem"))), apply_to=function)
void* declared(int fd, void* buf, unsigned long len, unsigned long offset);
#pragma clang attribute pop
void* declared(int fd, void* buf, unsigned long len, unsigned long offset)
{
    return (void*)(long)(fd + (buf != 0) + len + offset);
}

#pragma clang attribute push(__attribute__((annotate("filc_async", "runtime=io_uring", "op=fsync", "w_dep=fd:file"))), apply_to=function)
void* merged(int fd) { return (void*)(long)fd; }
#pragma clang attribute pop

void* separate(int fd);
#pragma clang attribute push(__attribute__((annotate("filc_async", "runtime=io_uring", "op=fsync", "r_dep=fd:file"))), apply_to=function)
void* separate(int fd) { return (void*)(long)fd; }
#pragma clang attribute pop

#pragma clang attribute push(__attribute__((annotate("filc_async", "runtime=io_uring", "op=close", "r_dep=fd:file"))), apply_to=function)
void* overridden(int fd);
#pragma clang attribute pop
#pragma clang attribute push(__attribute__((annotate("filc_async", "runtime=io_uring", "op=fsync", "w_dep=fd:file"))), apply_to=function)
void* overridden(int fd) { return (void*)(long)fd; }
#pragma clang attribute pop

#pragma clang attribute push(__attribute__((annotate("filc_async", "runtime=io_uring", "op=pread", "bout=buf", "r_dep=fd:slotA", "w_dep=buf:slotB"))), apply_to=function)
void* named(int fd, void* buf, unsigned long len, unsigned long offset)
{
    return (void*)(long)(fd + (buf != 0) + len + offset);
}
#pragma clang attribute pop

void* invoke(int fd, void* buf)
{
    return declared(fd, buf, 1, 0) == merged(fd) ?
           separate(fd) : overridden(fd) == named(fd, buf, 1, 0) ?
           overridden(fd) : named(fd, buf, 1, 0);
}
