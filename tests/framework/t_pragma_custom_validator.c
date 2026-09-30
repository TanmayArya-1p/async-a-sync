/* tests/framework/t_pragma_custom_validator.c -- the startup validator is replaceable,
 * and replacing it turns the startup abort into a per-task error.
 *
 * The pass emits a constructor that calls filc_async_validate_table, which
 * rejects an op the runtime does not know; t_pragma_unknownop checks that
 * abort. Here a constructor at priority 101, ahead of the pass constructor at
 * 65535, installs a validator that accepts everything. The program therefore
 * starts, and the unknown op fails per task with -EINVAL: the runtime still
 * checks the op when the task starts, so nothing unknown reaches the backend. */
#include <errno.h>
#include <stdio.h>

#include "filc_async.h"

static unsigned long validator_calls;

static bool permissive(const filc_async_meta* meta)
{
    (void)meta;
    validator_calls++;
    return true;
}

__attribute__((constructor(101))) static void install_validator(void)
{
    filc_async_set_validator(permissive);
}

/* Shaped exactly like a pread, so the only thing wrong is the op name. */
#pragma clang attribute push(__attribute__((annotate("filc_async", "runtime=io_uring", "op=somefutureop", "bout=buf"))), apply_to=function)
void* futread(int fd, void* buf, size_t len, unsigned long offset)
{
    (void)fd;
    (void)buf;
    (void)len;
    (void)offset;
    return 0;
}
#pragma clang attribute pop

int main(void)
{
    char buf[8] = { 0 };
    struct filc_async_result_s r = { 0 };
    r.pending = futread(1, buf, sizeof buf, 0);
    filc_async_wait(&r);

    int ok = validator_calls == 1 && r.state == 2 && r.result == -EINVAL;
    printf("T_PRAGMA_CUSTOM_VALIDATOR %s (validator calls=%lu state=%u "
           "result=%ld)\n",
           ok ? "PASS" : "FAIL", validator_calls, (unsigned)r.state, r.result);
    return ok ? 0 : 1;
}
