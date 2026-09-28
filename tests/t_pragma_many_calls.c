/* tests/t_pragma_many_calls.c -- handles are retired once their completion is
 * delivered.
 *
 * The runtime's task list and pending-mark registry must both bound work in
 * flight, not work over the program's life: a loop that calls an annotated
 * function and waits on it many times has to keep running without the list
 * growing. fill's bout= buffer is marked on every call and must be released by
 * the matching wait, and the wait must retire the handle, so a second poll of
 * it reports nothing (see filc_async.h).
 *
 * `noinline` + volatile sink as in t_pragma_ignore.c, so the def survives
 * with a real body for the pass to reference.
 */
#include <stdio.h>
#include <stdlib.h>
#include "filc_async.h"

#define CALLS 200

#pragma clang attribute push(__attribute__((annotate("filc_async", "op=ignore", "bout=0"))), apply_to=function)
__attribute__((noinline)) void* fill(char* out)
{
    volatile char sink = *out;
    (void)sink;
    return 0;
}
#pragma clang attribute pop

int main(void)
{
    int all_done = 1;
    int all_resolved = 1;
    int all_retired = 1;

    for (int i = 0; i < CALLS; i++) {
        char* out = malloc(16);
        struct filc_async_result_s r = { 0 };
        r.pending = fill(out);
        filc_async_wait(&r);
        all_done &= r.state != 1;
        all_resolved &= !filc_async_is_pending(out);
        all_retired &= !filc_async_poll(&r);
        free(out);
    }

    filc_async_stats stats;
    filc_async_get_stats(&stats);
    int counted = stats.tasks_submitted == CALLS;

    int ok = all_done && all_resolved && all_retired && counted;
    printf("T_PRAGMA_MANY_CALLS %s (done=%d resolved=%d retired=%d submitted=%lu)\n",
           ok ? "PASS" : "FAIL", all_done, all_resolved, all_retired,
           stats.tasks_submitted);
    return ok ? 0 : 1;
}
