/* tests/t_pragma_unknownop.c -- negative control: the RUNTIME is the
 * authority for the op set, not the compiler.
 *
 * The FilAsync pass accepts any op= value; the immediate-fail runtime's
 * startup validator (run from the pass-emitted ctor via
 * filc_async_validate_table) must REJECT the unrecognized op before main and
 * abort with "function cannot be registered on this runtime".
 *
 * Like t_pragma_ignore, the body avoids the -O1+ hazard (the optimizer runs
 * before the pass): noinline keeps the call site, the volatile sink keeps the
 * body non-pure. The annotation needs a pointer return so a rewrite would be
 * legal in principle; the rejection does not depend on the rewrite.
 */
#include <stdio.h>
#include "filc_async.h"

#pragma clang attribute push(__attribute__((annotate("filc_async", "op=somefutureop", "fd=0"))), apply_to=function)
__attribute__((noinline)) void* futcall(int fd)
{
    volatile int sink = fd;
    (void)sink;
    return 0;
}
#pragma clang attribute pop

int main(void)
{
    void* p = futcall(1);
    struct filc_async_result_s r = { 0 };
    r.pending = p;
    filc_async_wait(&r);
    return r.result ? 0 : 1;
}