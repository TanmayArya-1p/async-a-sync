/* tests/t_pragma_markpending.c -- the pass's buffer pending-marking contract.
 *
 * markfml is annotated bout=0 (out/result buffer) and buf=1 (direction pending,
 * treated as out by default) plus bin=2 (const input). The patched compiler
 * must:
 *   - emit filc_async_mark_pending(out) and filc_async_mark_pending(recv)
 *     before the staged submit: out and recv probe pending in main;
 *   - NOT mark const_in (bin= is a const input, never pending);
 *   - emit __filc_async_resolve_markfml(a, b, c) -- a wrapper of the
 *     annotation's own signature -- that calls mark_nonpending on the marked
 *     buffer args. Calling it clears out and recv but leaves const_in alone.
 *
 * The `noinline` + volatile sink conventions follow t_pragma_ignore.c: the
 * optimizer runs before FilAsyncPass, and the wrapper/impl are referenced by
 * pointer, so the defs must survive with a real body.
 */
#include <stdio.h>
#include <stdlib.h>
#include "filc_async.h"

#pragma clang attribute push(__attribute__((annotate("filc_async", "op=ignore", "bout=0", "buf=1", "bin=2"))), apply_to=function)
__attribute__((noinline)) void* markfml(char* out, char* recv, const char* const_in)
{
    volatile char sink = *out + *recv + *const_in;
    (void)sink;
    return 0;
}
#pragma clang attribute pop

/* The compiler-generated resolve wrapper; declared here, defined at link time
 * in the object the patched compiler emits for this TU. */
void __filc_async_resolve_markfml(char* out, char* recv, const char* const_in);

int main(void)
{
    char* out = malloc(64);
    char* recv = malloc(64);
    char* const_in = malloc(64);
    struct filc_async_result_s r = { 0 };

    r.pending = markfml(out, recv, const_in);
    filc_async_wait(&r);

    int marked_ok = filc_async_is_pending(out) && filc_async_is_pending(recv);
    int const_ok = !filc_async_is_pending(const_in);
    __filc_async_resolve_markfml(out, recv, const_in);
    int resolved_ok = !filc_async_is_pending(out) && !filc_async_is_pending(recv);

    printf("T_PRAGMA_MARKPENDING %s (marked=%d const=%d resolved=%d)\n",
           r.result && marked_ok && const_ok && resolved_ok ? "PASS" : "FAIL",
           marked_ok, const_ok, resolved_ok);
    free(out);
    free(recv);
    free(const_in);
    return (r.result && marked_ok && const_ok && resolved_ok) ? 0 : 1;
}