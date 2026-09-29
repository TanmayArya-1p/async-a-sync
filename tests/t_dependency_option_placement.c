// Dependency options follow the same declaration rules as op= and fd=.
// Every function is called so Clang retains its annotation. A `:<name>` suffix
// adds a namespace hash, so value-equal deps in different namespaces do not
// order each other.
#pragma clang attribute push(__attribute__((annotate("filc_async", "op=pread", "fd=0", "bout=1", "r_dep=0", "w_dep=1"))), apply_to=function)
void* declared(int fd, void* buf, unsigned long len, unsigned long offset);
#pragma clang attribute pop
void* declared(int fd, void* buf, unsigned long len, unsigned long offset)
{
    return (void*)(long)(fd + (buf != 0) + len + offset);
}

#pragma clang attribute push(__attribute__((annotate("filc_async", "op=fsync", "fd=0", "w_dep=0"))), apply_to=function)
void* merged(int fd) { return (void*)(long)fd; }
#pragma clang attribute pop

void* separate(int fd);
#pragma clang attribute push(__attribute__((annotate("filc_async", "op=fsync", "fd=0", "r_dep=0"))), apply_to=function)
void* separate(int fd) { return (void*)(long)fd; }
#pragma clang attribute pop

#pragma clang attribute push(__attribute__((annotate("filc_async", "op=close", "fd=0", "r_dep=0"))), apply_to=function)
void* overridden(int fd);
#pragma clang attribute pop
#pragma clang attribute push(__attribute__((annotate("filc_async", "op=fsync", "fd=0", "w_dep=0"))), apply_to=function)
void* overridden(int fd) { return (void*)(long)fd; }
#pragma clang attribute pop

#pragma clang attribute push(__attribute__((annotate("filc_async", "op=pread", "fd=0", "bout=1", "r_dep=0:slotA", "w_dep=1:slotB"))), apply_to=function)
void* named(int fd, void* buf, unsigned long len, unsigned long offset);
#pragma clang attribute pop
void* named(int fd, void* buf, unsigned long len, unsigned long offset)
{
    return (void*)(long)(fd + (buf != 0) + len + offset);
}

void* invoke(int fd, void* buf)
{
    return declared(fd, buf, 1, 0) == merged(fd) ?
           separate(fd) : overridden(fd) == named(fd, buf, 1, 0) ?
           overridden(fd) : named(fd, buf, 1, 0);
}