#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* Generic async-function interface.
 *
 * A function declared with `#pragma clang attribute` +
 * `__attribute__((annotate("filc_async", ...)))` is called through a stub the
 * FilAsync pass emits for it. The stub takes the call's dependency locks and
 * marks its output buffers pending through this framework, then hands the
 * call to the runtime (see filc_async_runtime.h). The framework never
 * interprets op= or argument shapes; which ops exist, and what they do, is up
 * to the linked runtime.
 *
 * filc_async_meta must match, byte for byte, the descriptor global the pass
 * emits (name at 0, nargs at 16, opts at 32, args[0] at 48 once the 16-byte
 * Fil-C pointers are in play). Do not change field order.
 *
 * Dependencies: repeat r_dep=<i> or w_dep=<i> on an annotated function
 * declaration or definition for each argument that names a dependency. They
 * follow the same placement rules as other options. The stub locks each such
 * argument's value before the call is handed to the runtime and the lock is
 * released when the call completes: a read lock is shared, a write lock is
 * exclusive, and a call waits in its stub until the locks it needs are free.
 * Scalar values are keys by value; pointer arguments are keys by object
 * identity. A :<name> suffix, as in w_dep=0:meta, puts the key in a
 * namespace: equal values in different namespaces are different resources,
 * and a key without a name is in a namespace of its own.
 *
 * Pending buffers: the stub marks the producing args (bout=, bare buf=, and
 * unannotated pointer args) pending; bin= inputs never are. Marking a buffer
 * another call still owns waits for that call. When a call completes, the
 * buffers it still has pending resolve; a runtime can resolve some earlier.
 *
 * Threading: every entry point below may be called from any thread, and a
 * runtime may report completions from any thread. A thread touching a buffer
 * another thread's call still owns waits for that call.
 */

#define FILC_ASYNC_RESULT_NONE 0u
#define FILC_ASYNC_RESULT_WORD 1u
#define FILC_ASYNC_RESULT_PTR  2u

/* Arg kinds. The pass records these from the pragma's positional tokens
 * ONLY -- op= never decides a kind: fd=<i> -> FD, bin=<i> -> BUFFER_IN,
 * bout=<i> -> BUFFER_OUT, buf=<i> -> PENDING (no direction annotated; the
 * runtime decides at use time). Unannotated pointer args also default to
 * PENDING (pessimistic); unannotated non-pointers stay IGNORED. */
#define FILC_ASYNC_ARG_IGNORED    0u
#define FILC_ASYNC_ARG_SCALAR     1u
#define FILC_ASYNC_ARG_BUFFER_IN  2u
#define FILC_ASYNC_ARG_BUFFER_OUT 3u
#define FILC_ASYNC_ARG_FD         4u
#define FILC_ASYNC_ARG_PENDING    5u

/* Dependency bits in args[i].dependency: read or write; the pointer bit,
 * which separates an object identity from a scalar with the same numeric
 * address; and a namespace in bits 8..31, a 24-bit hash of the <name> in
 * r_dep=<i>:<name> or w_dep=<i>:<name> (never 0), or 0 when the option names
 * none. Dependency options do not contribute to noped_args, which counts
 * fd=/bin=/bout=/buf= only. */
#define FILC_ASYNC_DEP_NONE            0u
#define FILC_ASYNC_DEP_READ            1u
#define FILC_ASYNC_DEP_WRITE           2u
#define FILC_ASYNC_DEP_POINTER         4u
#define FILC_ASYNC_DEP_NAMESPACE_SHIFT 8u
#define FILC_ASYNC_DEP_NAMESPACE_MASK  0x00FFFFFFu

typedef struct {
    const char* name;
    uint32_t    nargs;
    uint32_t    noped_args;
    uint32_t    flags;
    uint32_t    result;
    const char* const* opts;
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
 * it. With a NULL task the mark has no owner and only mark_resolved clears it. */
void  filc_async_mark_pending(void* task, void* buf);

/* ---- Program API ---- */

/* Delivering a completion (poll returning true, or wait) retires the handle:
 * `out` keeps the result, and later poll/wait calls on it are no-ops. */
bool  filc_async_poll(struct filc_async_result_s* out);
void  filc_async_wait(struct filc_async_result_s* out);

void  filc_async_mark_resolved(void* buf);
bool  filc_async_is_pending(const void* buf);

void  filc_async_get_stats(filc_async_stats* out);

/* Startup validation: the pass-emitted per-TU constructor calls
 * filc_async_validate_table before main. It checks the dependency bits, then
 * asks the runtime (filc_async_runtime_validate) whether it can run each
 * function. The pass constructor runs last (priority 65535), so a program's
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
