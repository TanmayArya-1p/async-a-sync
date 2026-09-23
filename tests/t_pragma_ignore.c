/* tests/t_pragma_ignore.c -- the link+run end-to-end proof for the
 * immediate-fail runtime.
 *
 * `somesuch` is annotated with op=ignore (Ruling-5: valid, never executed,
 * resolves instantly with -EOPNOTSUPP). The patched compiler rewrites the call
 * in main into staging + filc_async_submit, and the runtime resolves it via
 * wait -> -EOPNOTSUPP != 0 -> PASS. Ruling-2: the function returns a POINTER
 * (the pass's result-typing guard would refuse to rewrite an `int` return).
 *
 * somesuch is defined here (not just declared) because the pass passes the
 * renamed implementation `&__filc_async_somesuch` to submit as an argument,
 * and a mere declaration would leave that symbol undefined at link time. The
 * body is never called -- the rewrite replaces every call site.
 *
 * The body must not be a compile-time-removable pure computation: at -O1+ the
 * function passes AND the inliner run BEFORE FilAsyncPass. A body like
 * `return 0` lets SCCP prove the call is side-effect-free and delete it, and
 * a small body gets inlined into main -- either way the pass is left with no
 * call site to rewrite. The volatile sink keeps the body non-pure, and
 * `noinline` keeps the call site out of the inliner; once the rewrite runs,
 * main calls filc_async_submit instead and the body is never called at all.
 */
#include <stdio.h>
#include "filc_async.h"

#pragma clang attribute push(__attribute__((annotate("filc_async", "op=ignore"))), apply_to=function)
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