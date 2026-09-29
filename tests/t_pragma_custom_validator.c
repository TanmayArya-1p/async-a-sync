/* tests/t_pragma_custom_validator.c -- the startup validator is a replaceable
 * hook, and overriding it downgrades an abort to a per-task error.
 *
 * The pass emits a ctor calling filc_async_validate_table, which normally
 * rejects an op name the runtime does not know. t_pragma_unknownop is the
 * negative control for that. Here a ctor installs a replacement validator
 * first: it sits at priority 101, the pass ctor at 65535, so the override is
 * in place before the table is validated.
 *
 * The program therefore starts, and the unknown op fails per-task with -EINVAL
 * instead of aborting before main. The shape check in start_task() is the
 * second gate and is not overridable, so nothing unsafe reaches io_uring.
 */
#include <errno.h>
#include <stdio.h>
#include <stddef.h>

#include "filc_async.h"

static unsigned long validator_calls;

static bool permissive(const filc_async_meta* meta)
{
    (void)meta;
    validator_calls++;
    return true;
}

// Ahead of the pass ctor (65535), so the override is installed before validation.
__attribute__((constructor(101))) static void install_validator(void)
{
    filc_async_set_validator(permissive);
}

// Shaped exactly like a pread. Only the op name is unknown, so the pass emits
// the same argument kinds and the failure is attributable to the op name alone.
#pragma clang attribute push(__attribute__((annotate("filc_async", "op=somefutureop", "fd=0", "bout=1"))), apply_to=function)
__attribute__((noinline)) void* futread(int fd, void* buf, size_t len, unsigned long offset)
{
    volatile int sink = (int)fd;
    (void)sink;
    (void)buf;
    (void)len;
    (void)offset;
    return 0;
}
#pragma clang attribute pop

int main(void)
{
    char buf[8] = { 0 };
    void* task = futread(1, buf, sizeof buf, 0);
    struct filc_async_result_s r = { 0 };
    r.pending = task;
    filc_async_wait(&r);

    if (validator_calls != 1) {
        printf("FAIL: override ran %lu times, expected 1\n", validator_calls);
        return 1;
    }
    if (r.state != 2 || r.result != -EINVAL) {
        printf("FAIL: expected failed task with -EINVAL, got state=%u result=%ld\n",
               (unsigned)r.state, r.result);
        return 1;
    }
    printf("override honoured: unknown op rejected per-task with EINVAL\n");
    return 0;
}
