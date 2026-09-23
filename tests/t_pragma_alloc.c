/* tests/t_pragma_alloc.c -- allocator interface only.
 *
 * Uses ONLY the allocator functions (set/get_allocator), so it links with the
 * arena object alone -- no annotations, no submit, no backend symbols. Builds
 * with the stock filcc.
 *
 * Plan-sample corrections:
 *  1. The plan's `counting_alloc` delegated to filc_async_get_allocator(),
 *     which is the counting allocator itself once set_allocator ran -- an
 *     infinite recursion. We capture the DEFAULT arena in g_default BEFORE
 *     installing the counting one and delegate to that.
 *  2. We assert calls==1 && frees==0 (the one counting alloc routed to the
 *     default arena, nothing ever freed).
 */
#include <stdio.h>
#include "filc_async.h"
#include "filc_async_alloc.h"

static unsigned calls = 0, frees = 0;
static filc_async_allocator g_default;

static void* counting_alloc(size_t size, size_t align)
{
    calls++;
    return g_default.alloc(size, align);
}

static void counting_free(void* p, size_t size)
{
    frees++;
    g_default.free(p, size);
}

int main(void)
{
    /* Capture the default (arena) allocator BEFORE installing the counting
     * one, so the counting implementation can delegate without recursing. */
    g_default = filc_async_get_allocator();

    filc_async_allocator a = { counting_alloc, counting_free };
    filc_async_set_allocator(a);

    void* p = filc_async_get_allocator().alloc(32, 16);
    if (p == NULL)
        return 1;

    /* The default arena returns zeroed memory (zgc guarantees zero). */
    unsigned verify = 0;
    for (int i = 0; i < 32; i++)
        verify += ((unsigned char*)p)[i];
    if (verify != 0)
        return 2;

    /* Ruling-6: an all-zeros allocator means "restore the default arena". */
    filc_async_set_allocator((filc_async_allocator){ 0, 0 });
    void* q = filc_async_get_allocator().alloc(8, 8);
    if (q == NULL)
        return 3;

    if (calls != 1 || frees != 0) {
        printf("t_pragma_alloc FAIL: counting alloc calls=%u frees=%u\n",
               calls, frees);
        return 4;
    }
    printf("t_pragma_alloc PASS: counting alloc calls=%u frees=%u\n", calls, frees);
    return 0;
}