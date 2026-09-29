/* tests/t_unlinked_runtime.c -- a runtime the program does not link.
 *
 * run.sh builds this without any runtime named nosuch: the link must fail on
 * the undefined filc_async_runtime_nosuch descriptor, not start and fail at
 * run time. */
#pragma clang attribute push(__attribute__((annotate("filc_async", "runtime=nosuch", "op=noop"))), apply_to=function)
void* noop(void);
#pragma clang attribute pop

void* noop(void) { return 0; }

int main(void)
{
    noop();
    return 0;
}
