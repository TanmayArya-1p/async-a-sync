/* tests/framework/t_const_inference.c -- a pointer to const is an input.
 *
 * With no buffer option, a pointer parameter takes its kind from its type:
 * a pointer to const reads as bin= (never pending), any other pointer as
 * buf= (pending until the call completes). Only the pointee's const counts.
 * infer's parameters:
 *   in      const char*          an input: not marked
 *   names   const char* const*   an input: not marked
 *   tdef    cbyte*               const through a typedef: not marked
 *   out     char*                written: marked
 *   fixed   char* const          the pointer is const, the bytes are not:
 *                                marked
 *   list    const char**         the char* array is writable: marked
 *   forced  const char*          FILC_BUF overrides the type: marked
 *
 * Everything reads clear after wait(). Needs the patched clang: a stock
 * clang records no constness, and every pointer would be marked.
 */
#include <stdio.h>
#include <stdlib.h>
#include "filc_async.h"

typedef const char cbyte;

FILC_ASYNC(io_uring, FILC_OP(ignore), FILC_BUF(forced))
__attribute__((noinline)) void* infer(const char* in, const char* const* names,
                                      cbyte* tdef, char* out, char* const fixed,
                                      const char** list, const char* forced)
{
    volatile char sink = *in + **names + *tdef + *out + *fixed + **list + *forced;
    (void)sink;
    return 0;
}

int main(void)
{
    char* in = calloc(1, 64);
    const char** names = calloc(1, sizeof *names);
    char* tdef = calloc(1, 64);
    char* out = calloc(1, 64);
    char* fixed = calloc(1, 64);
    const char** list = calloc(1, sizeof *list);
    char* forced = calloc(1, 64);
    names[0] = in;
    list[0] = in;
    struct filc_async_result_s r = { 0 };

    r.pending = infer(in, names, tdef, out, fixed, list, forced);

    int inputs_ok = !filc_async_is_pending(in) && !filc_async_is_pending(names)
                    && !filc_async_is_pending(tdef);
    int outputs_ok = filc_async_is_pending(out) && filc_async_is_pending(fixed)
                     && filc_async_is_pending(list);
    int override_ok = filc_async_is_pending(forced);

    filc_async_wait(&r);

    int resolved_ok = !filc_async_is_pending(out) && !filc_async_is_pending(fixed)
                      && !filc_async_is_pending(list) && !filc_async_is_pending(forced);

    int ok = r.result && inputs_ok && outputs_ok && override_ok && resolved_ok;
    printf("T_CONST_INFERENCE %s (inputs=%d outputs=%d override=%d resolved=%d)\n",
           ok ? "PASS" : "FAIL", inputs_ok, outputs_ok, override_ok, resolved_ok);
    free(in);
    free(names);
    free(tdef);
    free(out);
    free(fixed);
    free(list);
    free(forced);
    return ok ? 0 : 1;
}
