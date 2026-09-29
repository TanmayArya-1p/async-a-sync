#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* Generic async-function interface.
 *
 * A function declared with `#pragma clang attribute` +
 * `__attribute__((annotate("filc_async", ...)))` has its call sites rewritten
 * by the FilAsync pass into filc_async_submit against this interface. The
 * io_uring backend supports op=pread, pwrite, openat, fsync and close using
 * their standard syscall argument order. op=ignore is a test-only operation
 * that completes with -EOPNOTSUPP.
 *
 * filc_async_meta must match, byte for byte, the descriptor global the pass
 * emits (name at 0, nargs at 16, opts at 32, args[0] at 48 once the 16-byte
 * Fil-C pointers are in play). Do not change field order.
 *
 * Ordering: independent submits do not wait for program order. A declared
 * dependency can make submit wait for an earlier conflicting call before it
 * queues the new request. Other dependencies are resolved lazily at the first
 * data access to a still-in-flight range (result buffers held by pending
 * ops); bare pointer passing does not synchronize, and two writers to one
 * buffer need explicit sequencing. A pending range always resolves.
 *
 * Threading: like the fasync_* API (see fasync.h), this is single-threaded;
 * every entry point below must be called from the ring's owner thread.
 *
 * Pending marks: submit marks the producing buffer args (bout=, bare buf=, and
 * unannotated pointer args) of an annotated call pending before it queues the
 * request; bin= const inputs are never marked. is_pending reports range
 * coverage; mark_resolved clears a mark (the runtime auto-resolves a task's
 * buffers on completion, and the program can resolve early between calls).
 * Marking a range already claimed by an older op resolves that op first.
 * Marks are object-range records, not object-flag bits. The io_uring backend
 * retains staged pointer capabilities until each request completes.
 *
 * Repeat r_dep=<i> or w_dep=<i> on an annotated function declaration or
 * definition for each argument that names a dependency. They follow the same
 * placement rules as other options. Scalar keys compare by value; pointer keys
 * compare by object identity. A :<name> suffix, as in w_dep=0:meta, puts the
 * key in a namespace: equal values in different namespaces are different
 * resources, and a key without a name is in a namespace of its own.
 * Equal-key read/read calls may overlap; every other pair dispatches in
 * submission order. Submit waits for a conflicting predecessor
 * before returning its task, so lazy output-buffer access can find the SQE.
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

/* Explicit result handle for poll()/wait(). `pending` is the pointer submit
 * returned; the rewritten call site hands it back to the caller, who parks it
 * here so poll/wait can find the task. Defined in the header (not opaque)
 * because tests read .result/.state directly. */
struct filc_async_result_s {
    const void* pending;   /* the pointer returned by submit, for wait() */
    long result;           /* bytes transferred / CQE res / -errno */
    unsigned char state;   /* 0 done, 1 pending, 2 failed */
};

typedef struct {
    unsigned long tasks_submitted;
    unsigned long tasks_completed;
    unsigned long tasks_failed;
    unsigned long sqes_queued;
    unsigned long kernel_submit_entries;
    unsigned long kernel_wait_entries;
    unsigned long pending_resolves; /* mark_pending resolved a stale mark */
} filc_async_stats;

/* Takes ownership of staged_args (the pass-emitted arg array: nargs filc_ptr
 * slots) and returns a task pointer for explicit poll/wait. Output-buffer
 * accesses also resolve the underlying io_uring request through Fil-C's
 * compiler-inserted access hook. */
void* filc_async_submit(const filc_async_meta* meta, void* impl, void* opts,
                        void* staged_args, size_t nargs);
/* Delivering a completion (poll returning true, or wait) retires the handle:
 * `out` keeps the result, and later poll/wait calls on it are no-ops. */
bool  filc_async_poll(struct filc_async_result_s* out);
void  filc_async_wait(struct filc_async_result_s* out);

// Buffer pending-marking. filc_async_submit marks the producing args (bout=,
// bare buf=, and unannotated pointers, all out-by-default) of an annotated call
// before queuing it; bin= const inputs are never marked. A program can also
// mark and resolve buffers of its own with these.
// Marking a range already held by an older op resolves that op first (generic
// gate), then re-marks. The runtime auto-resolves a task's marked buffers on
// completion; the program can resolve early with mark_resolved.
void  filc_async_mark_pending(void* buf);
void  filc_async_mark_resolved(void* buf);
bool  filc_async_is_pending(const void* buf);

void  filc_async_capabilities(unsigned long* syscall_shaped, unsigned long* executes_bodies);
void  filc_async_get_stats(filc_async_stats* out);

/* Startup validation: the pass-emitted per-TU constructor calls
 * filc_async_validate_table before main. The default validator checks that
 * the op is known and the arg kinds are consistent. The pass constructor runs
 * last (priority 65535), so a program's own constructor that calls
 * filc_async_set_validator is installed first and decides instead.
 *
 * This replaces the validator; it does not add an implementation for an op
 * name. The op set is closed in the runtime, so a permissive validator cannot
 * make an unknown op work, only stop the abort: the runtime re-checks the op
 * and shape when the task starts and fails it with -EINVAL. */
typedef bool (*filc_async_validator_fn)(const filc_async_meta* meta);
void  filc_async_set_validator(filc_async_validator_fn fn);
void  filc_async_validate_table(const filc_async_meta* const* metas); /* called by pass-generated ctor */
void  filc_async_fatal(const char* msg) __attribute__((noreturn));
