/* tests/t_pragma_markpending.c -- the pass's buffer pending-marking contract.
 *
 * markfml is annotated bout=0 (out/result buffer), buf=1 (direction pending,
 * out by default), bin=2 (const input), and extra=3 is left unannotated (so it
 * defaults to PENDING -- the pessimistic case). The patched compiler must:
 *   - emit filc_async_mark_pending(out), (recv), and (extra) before the staged
 *     submit: out, recv, and extra probe pending in main before wait;
 *   - NOT mark const_in (bin= is a const input, never pending);
 *   - let wait() auto-resolve the marked buffers, so all four read clear after.
 *
 * The `noinline` + volatile sink conventions follow t_pragma_ignore.c: the
 * optimizer runs before FilAsyncPass, and the impl is referenced by pointer,
 * so the def must survive with a real body.
 */
#include <stdio.h>
#include <stdlib.h>
#include "filc_async.h"

#pragma clang attribute push(__attribute__((annotate("filc_async", "op=ignore", "bout=0", "buf=1", "bin=2"))), apply_to=function)
__attribute__((noinline)) void* markfml(char* out, char* recv, const char* const_in, char* extra)
{
    volatile char sink = *out + *recv + *const_in + *extra;
    (void)sink;
    return 0;
}
#pragma clang attribute pop

int main(void)
{
    char* out = malloc(64);
    char* recv = malloc(64);
    char* const_in = malloc(64);
    char* extra = malloc(64);
    struct filc_async_result_s r = { 0 };

    r.pending = markfml(out, recv, const_in, extra);

    int marked_ok = filc_async_is_pending(out) && filc_async_is_pending(recv)
                    && filc_async_is_pending(extra);
    int const_ok = !filc_async_is_pending(const_in);

    filc_async_wait(&r);

    int resolved_ok = !filc_async_is_pending(out) && !filc_async_is_pending(recv)
                      && !filc_async_is_pending(extra) && !filc_async_is_pending(const_in);

    printf("T_PRAGMA_MARKPENDING %s (marked=%d const=%d resolved=%d)\n",
           r.result && marked_ok && const_ok && resolved_ok ? "PASS" : "FAIL",
           marked_ok, const_ok, resolved_ok);
    free(out);
    free(recv);
    free(const_in);
    free(extra);
    return (r.result && marked_ok && const_ok && resolved_ok) ? 0 : 1;
}
