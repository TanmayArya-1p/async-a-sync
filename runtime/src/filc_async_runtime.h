#pragma once

/* The contract between the generic framework and a runtime.
 *
 * The pass emits a stub for every annotated function. The stub takes the
 * call's dependency locks and marks its output buffers pending through the
 * framework, then hands the call to the runtime with filc_async_submit. The
 * framework never looks at op= or at argument shapes; everything op-specific
 * happens behind filc_async_submit. A runtime implements the three functions
 * below and may call the framework services after them.
 *
 * A runtime that runs each body on a worker thread would implement submit by
 * queuing `run` and staged args, and report the body's return value with
 * filc_async_complete when it finishes. */

#include <stdbool.h>
#include <stddef.h>

#include "filc_async.h"

/* Calls the annotated function's body with the staged arguments and returns
 * its result as a word. Emitted by the pass; only runtimes call it, through
 * filc_async_run. */
typedef long (*filc_async_run_fn)(void* staged_args);

/* ---- Implemented by the runtime ---- */

/* Starts `task`. The call has already taken its locks and marked its buffers.
 * `run` may be NULL for a task submitted without a body. The runtime reports
 * the outcome with filc_async_complete, now or later. */
void filc_async_submit(void* task, const filc_async_meta* meta,
                       filc_async_run_fn run, void* staged_args, size_t nargs);

/* How far filc_async_runtime_poll may go for a task still running. */
enum filc_async_poll_mode {
    FILC_ASYNC_POLL_CHECK,    /* only look: start nothing, never block */
    FILC_ASYNC_POLL_PROGRESS, /* may start deferred work, never block */
    FILC_ASYNC_POLL_BLOCK     /* return only once the task has completed */
};

/* Asks the runtime about `task`. Returns true once filc_async_complete has
 * been called for it. */
bool filc_async_runtime_poll(void* task, enum filc_async_poll_mode mode);

/* Whether the runtime can run calls described by `meta`: the startup check
 * for every annotated function, unless the program installed its own
 * validator with filc_async_set_validator. */
bool filc_async_runtime_validate(const filc_async_meta* meta);

/* ---- Framework services for runtimes ---- */

/* Reports that `task` finished with `result` (-errno on failure): resolves
 * the buffers it still has pending and releases its dependency locks. */
void filc_async_complete(void* task, long result);

/* Runs the body of `task`. While it runs, the body may use its own pending
 * buffers without waiting for itself. Returns the body's result, or 0 when
 * `run` is NULL. */
long filc_async_run(void* task, filc_async_run_fn run, void* staged_args);

/* Resolves one of `task`'s buffers before the task completes. A buffer that
 * `task` does not own is left alone. */
void filc_async_resolve_buffer(void* task, void* buf);

/* One word of per-task storage for the runtime's own state. */
void** filc_async_task_runtime_data(void* task);
