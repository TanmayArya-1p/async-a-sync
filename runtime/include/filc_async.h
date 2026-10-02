#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "filc_async_annotate.h"

/* Generic async-function interface.
 *
 * A function annotated with FILC_ASYNC (filc_async_annotate.h), or with
 * `#pragma clang attribute` + `__attribute__((annotate("filc_async", ...)))`,
 * is called through a stub the
 * FilAsync pass emits for it. The stub takes the call's dependency locks and
 * marks its output buffers pending through this framework, then hands the
 * call to the runtime its runtime=<name> option names (see
 * filc_async_runtime.h). The framework never interprets op= or argument
 * shapes; which ops exist, and what they do, is up to that runtime. A program
 * may use several runtimes and links each one it names.
 *
 * filc_async_meta must match, byte for byte, the descriptor global the pass
 * emits (name at 0, nargs at 16, opts at 32, runtime at 48, args[0] at 64
 * once the 16-byte Fil-C pointers are in play). Do not change field order.
 *
 * Dependencies: repeat r_dep=<param>:<ns> or w_dep=<param>:<ns> on an
 * annotated function declaration or definition for each parameter that names
 * a dependency. They follow the same placement rules as other options. The
 * stub locks each such argument before the call is handed to the runtime and
 * the lock is released when the call completes: a read lock is shared, a
 * write lock is exclusive, and a call waits in its stub until the locks it
 * needs are free. The key is the argument's value (a scalar's value, or the
 * object a pointer points into) together with the parameter's name and the
 * namespace, so r_dep=fd:file and w_dep=fd:file conflict on equal fds while
 * w_dep=fd:meta does not.
 *
 * Pending buffers: the stub marks the producing args (bout=, bare buf=, and
 * unannotated pointers to non-const) pending, with write marks; inputs (bin=,
 * and unannotated pointers to const) get read marks, which are not pending
 * but which the program's own accesses still wait for, so a store never
 * changes what a queued call is still reading (read-only objects excepted:
 * they cannot be stored into). Marking a buffer for writing waits for every
 * other call that owns it, readers included, so a later call never writes a
 * buffer an earlier call is still reading. A read mark never waits. When a
 * call completes, its marks are released; a runtime can resolve a buffer
 * earlier.
 *
 * Threading: every entry point below may be called from any thread, and a
 * runtime may report completions from any thread. A thread touching a buffer
 * another thread's call still owns waits for that call.
 */

#define FILC_ASYNC_RESULT_NONE 0u
#define FILC_ASYNC_RESULT_WORD 1u
#define FILC_ASYNC_RESULT_PTR  2u

/* Arg kinds. The pass records these from the pragma's argument options and
 * the parameter types -- op= never decides a kind: bin=<p> -> BUFFER_IN,
 * bout=<p> -> BUFFER_OUT, buf=<p> -> PENDING (no direction annotated; the
 * runtime decides at use time). An unannotated pointer to const is
 * BUFFER_IN, and any other unannotated pointer PENDING (pessimistic);
 * unannotated non-pointers stay IGNORED. Value 4 is unused. */
#define FILC_ASYNC_ARG_IGNORED    0u
#define FILC_ASYNC_ARG_SCALAR     1u
#define FILC_ASYNC_ARG_BUFFER_IN  2u
#define FILC_ASYNC_ARG_BUFFER_OUT 3u
#define FILC_ASYNC_ARG_PENDING    5u

/* Dependency bits in args[i].dependency: read or write; the pointer bit,
 * which separates an object identity from a scalar with the same numeric
 * address; and a space in bits 8..31, a 24-bit FNV-1a hash of the
 * "<param>:<ns>" in r_dep=<param>:<ns> or w_dep=<param>:<ns> (never 0). A
 * collision only makes unrelated calls wait for each other. Dependency options do not contribute to noped_args, which counts
 * bin=/bout=/buf= only. */
#define FILC_ASYNC_DEP_NONE            0u
#define FILC_ASYNC_DEP_READ            1u
#define FILC_ASYNC_DEP_WRITE           2u
#define FILC_ASYNC_DEP_POINTER         4u
#define FILC_ASYNC_DEP_NAMESPACE_SHIFT 8u
#define FILC_ASYNC_DEP_NAMESPACE_MASK  0x00FFFFFFu

struct filc_async_runtime;

typedef struct {
    const char* name;
    uint32_t    nargs;
    uint32_t    noped_args;
    uint32_t    flags;
    uint32_t    result;
    const char* const* opts;
    const struct filc_async_runtime* runtime; /* from runtime=<name> */
    struct {
        uint32_t kind;
        uint32_t dependency;
    } args[];
} filc_async_meta;

/* Explicit result handle for poll()/wait(). `pending` is the task an
 * annotated call returned. Defined in the header (not opaque) because tests
 * read .result/.state directly. */
struct filc_async_result_s {
    const void* pending;   /* the task an annotated call returned */
    long result;           /* the runtime's result, or -errno */
    unsigned char state;   /* 0 done, 1 pending, 2 failed */
};

typedef struct {
    unsigned long tasks_submitted;
    unsigned long tasks_completed;
    unsigned long tasks_failed;
    unsigned long pending_resolves; /* marking waited for a buffer's owner */
    unsigned long lock_waits;       /* a lock waited for a conflicting call */
    unsigned long hook_resolves;    /* an access found its object pending */
} filc_async_stats;

/* ---- Called by the stubs the pass emits ---- */

/* Starts describing one annotated call. staged_args is the pass-emitted
 * array of nargs 16-byte cells. Returns the task handle. */
void* filc_async_begin(const filc_async_meta* meta, void* staged_args);
/* Takes a dependency lock on a scalar value or on the object `ptr` points
 * into. `space` is the argument's dependency bits without the mode, and
 * `mode` is FILC_ASYNC_DEP_READ or FILC_ASYNC_DEP_WRITE. */
void  filc_async_lock_word(void* task, uint64_t value, uint32_t space,
                           uint32_t mode);
void  filc_async_lock_ptr(void* task, const void* ptr, uint32_t space,
                          uint32_t mode);
/* Marks `buf` pending for `task`, first waiting for any other call that owns
 * it, including calls still reading it. With a NULL task the mark has no
 * owner and only mark_resolved clears it. */
void  filc_async_mark_pending(void* task, void* buf);
/* Records that `task` reads `buf` until it completes, without waiting and
 * without making `buf` pending: a later mark_pending on it, and the
 * program's own access to it, wait for `task`. Does nothing if `task`
 * already marked `buf`. */
void  filc_async_mark_input(void* task, const void* buf);

/* Calls the annotated function's body with the staged arguments and returns
 * its result as a word. Emitted by the pass; only runtimes call it, through
 * filc_async_run. */
typedef long (*filc_async_run_fn)(void* staged_args);

/* Hands the call to meta->runtime's submit. */
void  filc_async_submit(void* task, const filc_async_meta* meta,
                        filc_async_run_fn run, void* staged_args, size_t nargs);

/* ---- Program API ---- */

/* Delivering a completion (poll returning true, or wait) retires the handle:
 * `out` keeps the result, and later poll/wait calls on it are no-ops. */
bool  filc_async_poll(struct filc_async_result_s* out);
void  filc_async_wait(struct filc_async_result_s* out);

/* Pending means still being produced: read marks do not count. */
void  filc_async_mark_resolved(void* buf);
bool  filc_async_is_pending(const void* buf);

// not const: a call that takes a tag completes it, so an unannotated tag
// parameter is marked pending like any pointer to non-const
typedef void* prov_tag;
// one fil-c object per tag; null on failure
void* prov_alloc(void);

// returns a separate completion object without waiting for the inputs
// captures current marks; later marks on an input are not included
// null inputs and an empty array are allowed; errors stay with each task
// the returned object is gc-managed; do not free it or use it as data
void* filc_async_wait_all(const prov_tag* buffers, size_t count);

void  filc_async_get_stats(filc_async_stats* out);

/* Startup validation: the pass-emitted per-TU constructor calls
 * filc_async_validate_table before main. It checks the dependency bits, then
 * asks each function's runtime (its validate function) whether it can run
 * the function. The pass constructor runs last (priority 65535), so a program's
 * own constructor that calls filc_async_set_validator is installed first and
 * decides instead of the runtime.
 *
 * This replaces the validator; it does not add an implementation for an op
 * name. A permissive validator cannot make an op the runtime does not know
 * work, only stop the abort: the runtime rejects such a call when it is
 * submitted. */
typedef bool (*filc_async_validator_fn)(const filc_async_meta* meta);
void  filc_async_set_validator(filc_async_validator_fn fn);
void  filc_async_validate_table(const filc_async_meta* const* metas); /* called by pass-generated ctor */
void  filc_async_fatal(const char* msg) __attribute__((noreturn));
