/* tests/t_mock_runtime.c -- annotated calls with another runtime linked.
 *
 * Built with tests/mock_runtime.c instead of the io_uring runtime, which is
 * not linked at all. The op name means nothing to the framework, the runtime
 * runs the body and returns its result, and the body writes its own output
 * buffer, which its stub marked pending, without waiting for itself. */
#include <stdio.h>

#include "filc_async.h"

static volatile int body_calls;

#pragma clang attribute push(__attribute__((annotate("filc_async", "runtime=mock", "op=double", "bout=out", "bin=in", "w_dep=out:mem"))), apply_to=function)
void* double_into(long* out, const long* in)
{
    body_calls++;
    *out = *in * 2;
    return (void*)(*in * 2);
}
#pragma clang attribute pop

int main(void)
{
    static long in = 21;
    static long out;
    static long again;

    struct filc_async_result_s r = { 0 };
    r.pending = double_into(&out, &in);
    filc_async_wait(&r);

    /* No wait: the output is read directly. */
    double_into(&again, &out);
    long direct = again;

    filc_async_stats stats;
    filc_async_get_stats(&stats);
    int ok = r.state == 0 && r.result == 42 && out == 42 && direct == 84 &&
             body_calls == 2 && !filc_async_is_pending(&out) &&
             stats.tasks_submitted == 2 && stats.tasks_completed == 2;
    printf("T_MOCK_RUNTIME %s (result=%ld out=%ld direct=%ld body calls=%d)\n",
           ok ? "PASS" : "FAIL", r.result, out, direct, body_calls);
    return ok ? 0 : 1;
}
