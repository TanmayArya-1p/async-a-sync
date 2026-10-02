#pragma once

/* The contract between the generic framework and a runtime.
 *
 * The pass emits a stub for every annotated function. The stub takes the
 * call's dependency locks and marks its output buffers pending through the
 * framework, then hands the call to the runtime the function's
 * runtime=<name> option names. The framework never looks at op= or at
 * argument shapes; everything op-specific happens in the runtime.
 *
 * A runtime is a filc_async_runtime descriptor named
 * filc_async_runtime_<name>, defined with FILC_ASYNC_RUNTIME: its submit,
 * poll and validate functions. A program links every runtime its
 * annotations name, and each call goes to its own function's runtime; a
 * runtime that is not linked leaves filc_async_runtime_<name> undefined.
 *
 * A runtime that runs each body on a worker thread would implement submit by
 * queuing `run` and staged args, and report the body's return value with
 * filc_async_complete when it finishes. */

#include <stdbool.h>
#include <stddef.h>

#include "filc_async.h"

/* How far a runtime's poll may go for a task still running. */
enum filc_async_poll_mode {
    FILC_ASYNC_POLL_CHECK,    /* only look: start nothing, never block */
    FILC_ASYNC_POLL_PROGRESS, /* may start deferred work, never block */
    FILC_ASYNC_POLL_BLOCK     /* return only once the task has completed */
};

typedef struct filc_async_runtime {
    /* The <name> in runtime=<name>. */
    const char* name;

    /* Starts `task`. The call has already taken its locks and marked its
     * buffers. `run` may be NULL for a task submitted without a body. The
     * runtime reports the outcome with filc_async_complete, now or later. */
    void (*submit)(void* task, const filc_async_meta* meta,
                   filc_async_run_fn run, void* staged_args, size_t nargs);

    /* Asks about `task`. Returns true once filc_async_complete has been
     * called for it. */
    bool (*poll)(void* task, enum filc_async_poll_mode mode);

    /* Whether the runtime can run calls described by `meta`: the startup
     * check for each annotated function that names this runtime, unless the
     * program installed its own validator with filc_async_set_validator. */
    bool (*validate)(const filc_async_meta* meta);
} filc_async_runtime;

/* Defines the descriptor of the runtime `name`, the one runtime=<name>
 * annotations refer to. */
#define FILC_ASYNC_RUNTIME(name, submit, poll, validate)                  \
    const filc_async_runtime filc_async_runtime_##name = {                \
        #name, (submit), (poll), (validate)                               \
    }

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

/* Marks `buf` pending for `task`, as a write mark, without waiting for its
 * other owners, so several of a runtime's requests can own one object. */
void filc_async_mark_shared(void* task, void* buf);

/* Waits until no call other than `task` (which may be NULL) is producing the
 * object `buf` points into: for a runtime about to hand the kernel a buffer
 * an earlier call may still be filling. Calls that only read it are not
 * waited for. */
void filc_async_wait_buffer(void* task, const void* buf);

/* One word of per-task storage for the runtime's own state. */
void** filc_async_task_runtime_data(void* task);

/* Starts a task of runtime `rt` that did not come through a stub, for
 * requests a runtime makes itself. It is polled through `rt` and completed
 * like any other, but has no descriptor and is not counted in the stats. */
void* filc_async_task_new(const filc_async_runtime* rt);
