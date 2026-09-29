// tests/t_pending_registry.c -- the runtime's pending-marker contract.
// mark_pending puts a buffer in the pending set; mark_resolved removes it;
// is_pending reports range coverage, so aliases of a marked buffer probe true.

#include <stdio.h>
#include <stdlib.h>

#include "filc_async.h"

static int failures;

static void check(const char* what, int ok)
{
    printf("  %-44s %s\n", what, ok ? "ok" : "FAIL");
    fflush(stdout);
    if (!ok)
        failures++;
}

int main(void)
{
    char* a = malloc(64);
    char* b = malloc(64);
    filc_async_stats stats0;
    filc_async_stats stats1;
    unsigned long pending_resolves_before;

    check("not pending before mark", !filc_async_is_pending(a));
    filc_async_mark_pending(NULL, a);
    check("pending after mark", filc_async_is_pending(a));
    check("alias of a pending buffer probes pending", filc_async_is_pending(a + 8));
    check("unmarked buffer not pending", !filc_async_is_pending(b));
    filc_async_mark_resolved(a);
    check("not pending after clear", !filc_async_is_pending(a));
    filc_async_mark_resolved(a);
    check("double clear is harmless", !filc_async_is_pending(a));

    filc_async_mark_pending(NULL, a);
    filc_async_mark_pending(NULL, b);
    filc_async_mark_resolved(a);
    check("clearing one leaves the other", filc_async_is_pending(b) && !filc_async_is_pending(a));

    // b is still marked from above; re-marking it resolves the stale mark
    // first, so the requeue leaves ONE entry: a single clear releases it.
    filc_async_get_stats(&stats0);
    pending_resolves_before = stats0.pending_resolves;
    filc_async_mark_pending(NULL, b);
    check("requeue of a pending buffer stays pending", filc_async_is_pending(b));
    filc_async_get_stats(&stats1);
    check("requeue counted as a pending resolve",
          stats1.pending_resolves == pending_resolves_before + 1);
    filc_async_mark_resolved(b);
    check("one clear fully releases a requeued buffer", !filc_async_is_pending(b));

    free(a);
    free(b);

    printf("T_PENDING_REGISTRY %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}