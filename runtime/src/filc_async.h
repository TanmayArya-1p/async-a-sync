#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* Generic async-function interface.
 *
 * A function declared with `#pragma clang attribute` +
 * `__attribute__((annotate("filc_async", ...)))` has its call sites rewritten
 * by the FilAsync pass into filc_async_submit against this interface. The
 * only backend implemented so far is the immediate-fail placeholder in
 * filc_async.c (the io_uring runtime is a later branch).
 *
 * filc_async_meta must match, byte for byte, the descriptor global the pass
 * emits (name at 0, nargs at 16, opts at 32, args[0] at 48 once the 16-byte
 * Fil-C pointers are in play). Do not change field order.
 */

#define FILC_ASYNC_RESULT_NONE 0u
#define FILC_ASYNC_RESULT_WORD 1u
#define FILC_ASYNC_RESULT_PTR  2u

/* Arg kinds. The pass records these from the pragma's positional tokens
 * ONLY -- op= never decides a kind: fd=<i> -> FD, bin=<i> -> BUFFER_IN,
 * bout=<i> -> BUFFER_OUT, buf=<i> -> PENDING (no direction annotated; the
 * runtime decides at use time). Any other option index stays IGNORED. */
#define FILC_ASYNC_ARG_IGNORED    0u
#define FILC_ASYNC_ARG_SCALAR     1u
#define FILC_ASYNC_ARG_BUFFER_IN  2u
#define FILC_ASYNC_ARG_BUFFER_OUT 3u
#define FILC_ASYNC_ARG_FD         4u
#define FILC_ASYNC_ARG_PENDING    5u

typedef struct {
    const char* name;
    uint32_t    nargs;
    uint32_t    noped_args;
    uint32_t    flags;
    uint32_t    result;
    const char* const* opts;
    struct {
        uint32_t kind;
        uint32_t size;
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
} filc_async_stats;

/* Takes ownership of staged_args (the pass-emitted arg array: nargs filc_ptr
 * slots) and returns a pending pointer the caller dereferences -- or parks in
 * a filc_async_result_s for explicit poll/wait. */
void* filc_async_submit(const filc_async_meta* meta, void* impl, void* opts,
                        void* staged_args, size_t nargs);
bool  filc_async_poll(struct filc_async_result_s* out);
void  filc_async_wait(struct filc_async_result_s* out);

void  filc_async_capabilities(unsigned long* syscall_shaped, unsigned long* executes_bodies);
void  filc_async_get_stats(filc_async_stats* out);

/* Startup validation: the pass-emitted per-TU constructor calls
 * filc_async_validate_table before main. A program may install its own
 * validator with filc_async_set_register_fn; the default checks the op is
 * known and the arg kinds are consistent. */
typedef bool (*filc_async_register_fn)(const filc_async_meta* meta);
void  filc_async_set_register_fn(filc_async_register_fn fn);
void  filc_async_validate_table(const filc_async_meta* const* metas); /* called by pass-generated ctor */
void  filc_async_fatal(const char* msg) __attribute__((noreturn));