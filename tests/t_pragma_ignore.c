/* tests/t_pragma_ignore.c -- link+run proof for the unsupported-op path.
 *
 * `somesuch` is annotated op=ignore (valid, never executed, resolves instantly
 * with -EOPNOTSUPP). The patched compiler rewrites the call in main into
 * staging + filc_async_submit; wait() -> -EOPNOTSUPP != 0 -> PASS. The
 * function returns a POINTER (a non-pointer return would be left in place).
 *
 * somesuch is defined here (not just declared) because submit receives the
 * renamed implementation `&__filc_async_somesuch`; a mere declaration would
 * leave that symbol undefined at link time. The body is never called.
 *
 * FilAsyncPass runs before the optimizer, so even a pure body could not be
 * deleted or inlined ahead of the rewrite (t_pragma_same_tu_lazy checks
 * that). The volatile sink and `noinline` date from when it ran afterwards and
 * are kept as belt and braces.
 */
#include <stdio.h>
#include "filc_async.h"

#pragma clang attribute push(__attribute__((annotate("filc_async", "runtime=io_uring", "op=ignore"))), apply_to=function)
__attribute__((noinline)) void* somesuch(int a, int b)
{
    volatile int sink = a + b;
    (void)sink;
    return 0;
}
#pragma clang attribute pop

int main(void)
{
    struct filc_async_result_s r = { 0 };
    void* p = somesuch(1, 2);
    r.pending = p;
    filc_async_wait(&r);
    printf("T_PRAGMA_IGNORE %s (res=%ld)\n", r.result ? "PASS" : "FAIL", r.result);
    return r.result ? 0 : 1;
}
