/* tests/t_pragma_markpending.c -- the pass's buffer pending-marking contract.
 *
 * markfml is annotated bin=0 (buffer-in) with a bare buf=1 (direction-pending,
 * ARG_PENDING). The patched compiler must:
 *   - emit filc_async_mark_pending(bin) and filc_async_mark_pending(uabuf)
 *     before the staged submit, so both buffers probe pending in main;
 *   - emit __filc_async_resolve_markfml(a, b) -- a wrapper of the annotation's
 *     own signature -- that calls filc_async_mark_nonpending on each buffer
 *     arg. Calling it un-marks both buffers.
 *
 * The `noinline` + volatile sink conventions follow t_pragma_ignore.c: the
 * optimizer runs before FilAsyncPass, and the wrapper/impl are referenced by
 * pointer, so the defs must survive with a real body.
 */
#include <stdio.h>
#include <stdlib.h>
#include "filc_async.h"

#pragma clang attribute push(__attribute__((annotate("filc_async", "op=ignore", "bin=0", "buf=1"))), apply_to=function)
__attribute__((noinline)) void* markfml(char* a, char* b)
{
    volatile char sink = *a + *b;
    (void)sink;
    return 0;
}
#pragma clang attribute pop

/* The compiler-generated resolve wrapper; declared here, defined at link time
 * in the object the patched compiler emits for this TU. */
void __filc_async_resolve_markfml(char* a, char* b);

int main(void)
{
    char* bin = malloc(64);
    char* uabuf = malloc(64);
    struct filc_async_result_s r = { 0 };

    r.pending = markfml(bin, uabuf);
    filc_async_wait(&r);

    int pending_ok = filc_async_is_pending(bin) && filc_async_is_pending(uabuf);
    __filc_async_resolve_markfml(bin, uabuf);
    int resolved_ok = !filc_async_is_pending(bin) && !filc_async_is_pending(uabuf);

    printf("T_PRAGMA_MARKPENDING %s (pending=%d resolved=%d)\n",
           r.result && pending_ok && resolved_ok ? "PASS" : "FAIL",
           pending_ok, resolved_ok);
    free(bin);
    free(uabuf);
    return (r.result && pending_ok && resolved_ok) ? 0 : 1;
}