// tests/t_pending_registry.c -- the runtime's pending-marker contract.
// mark_pending puts a buffer in the pending set; mark_nonpending removes it;
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

    check("not pending before mark", !filc_async_is_pending(a));
    filc_async_mark_pending(a);
    check("pending after mark", filc_async_is_pending(a));
    check("alias of a pending buffer probes pending", filc_async_is_pending(a + 8));
    check("unmarked buffer not pending", !filc_async_is_pending(b));
    filc_async_mark_nonpending(a);
    check("not pending after clear", !filc_async_is_pending(a));
    filc_async_mark_nonpending(a);
    check("double clear is harmless", !filc_async_is_pending(a));

    filc_async_mark_pending(a);
    filc_async_mark_pending(b);
    filc_async_mark_nonpending(a);
    check("clearing one leaves the other", filc_async_is_pending(b) && !filc_async_is_pending(a));

    free(a);
    free(b);

    printf("T_PENDING_REGISTRY %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}