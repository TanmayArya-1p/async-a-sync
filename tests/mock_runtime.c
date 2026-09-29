/* tests/mock_runtime.c -- the smallest runtime behind the framework.
 *
 * It implements the three functions of filc_async_runtime.h and nothing
 * else: submit runs the call's body on the calling thread and reports its
 * return value as the result, poll has nothing left to do, and every function
 * is accepted. t_mock_runtime links it in place of the io_uring runtime. */
#include "filc_async_runtime.h"

void filc_async_submit(void* task, const filc_async_meta* meta,
                       filc_async_run_fn run, void* staged_args, size_t nargs)
{
    (void)meta;
    (void)nargs;
    filc_async_complete(task, filc_async_run(task, run, staged_args));
}

bool filc_async_runtime_poll(void* task, enum filc_async_poll_mode mode)
{
    (void)task;
    (void)mode;
    return true;
}

bool filc_async_runtime_validate(const filc_async_meta* meta)
{
    (void)meta;
    return true;
}
