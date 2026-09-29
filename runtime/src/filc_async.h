#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

// Reference: wiki/2026-09-29-pragma-async-runtime-interface.md
// Interface for calls annotated with the filc_async pragma. The compiler pass
// rewrites every call site into filc_async_submit.

// Result kinds.
#define FILC_ASYNC_RESULT_NONE 0u
#define FILC_ASYNC_RESULT_WORD 1u
#define FILC_ASYNC_RESULT_PTR  2u

// Arg kinds, from the positional options only. op= never sets a kind.
#define FILC_ASYNC_ARG_IGNORED    0u
#define FILC_ASYNC_ARG_SCALAR     1u
#define FILC_ASYNC_ARG_BUFFER_IN  2u
#define FILC_ASYNC_ARG_BUFFER_OUT 3u
#define FILC_ASYNC_ARG_FD         4u
#define FILC_ASYNC_ARG_PENDING    5u

// Dependency bits: flags, plus a resource namespace hashed into bits 8..31
// where 0 means unnamed.
#define FILC_ASYNC_DEP_NONE              0u
#define FILC_ASYNC_DEP_READ              1u
#define FILC_ASYNC_DEP_WRITE             2u
#define FILC_ASYNC_DEP_POINTER           4u
#define FILC_ASYNC_DEP_NAMESPACE_SHIFT   8u
#define FILC_ASYNC_DEP_NAMESPACE_MASK    0x00FFFFFFu

// Must match the descriptor global the pass emits, byte for byte. Do not
// reorder.
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

// Staged-argument cell. 16 bytes, not 8: a Fil-C capability has no inline
// value to stage in the second word, and an 8-byte stride would read the next
// cell's padding as an argument.
typedef union {
    void* ptr;
    uint64_t word;
} filc_async_arg_value;

typedef struct {
    filc_async_arg_value value;
    uint64_t pad;
} filc_async_arg;

// Not opaque: tests read .result and .state directly.
struct filc_async_result_s {
    const void* pending;   // task pointer from submit, for wait()
    long result;           // bytes transferred, CQE res, or -errno
    unsigned char state;   // 0 done, 1 pending, 2 failed
};

typedef struct {
    unsigned long tasks_submitted;
    unsigned long tasks_completed;
    unsigned long tasks_failed;
    unsigned long sqes_queued;
    unsigned long kernel_submit_entries;
    unsigned long kernel_wait_entries;
    unsigned long pending_resolves; // stale marks resolved, lazy hook excluded
} filc_async_stats;

// Takes ownership of the pass-emitted staged args and returns a task pointer.
void* filc_async_submit(const filc_async_meta* meta, void* impl, void* opts,
                        void* staged_args, size_t nargs);
bool  filc_async_poll(struct filc_async_result_s* out);
void  filc_async_wait(struct filc_async_result_s* out);

// Range pending marks. The stub marks the producing args of an annotated call;
// bin= inputs are never marked. Marking a range held by an older op resolves
// that op first.
void  filc_async_mark_pending(void* buf);
void  filc_async_mark_resolved(void* buf);
bool  filc_async_is_pending(const void* buf);

void  filc_async_capabilities(unsigned long* syscall_shaped, unsigned long* executes_bodies);
void  filc_async_get_stats(filc_async_stats* out);

// Startup validation, called by the pass-generated constructor before main.
// The pass ctor runs last (priority 65535), so a constructor of your own that
// calls filc_async_set_validator is installed first and gets to decide.
//
// This replaces the validator; it does not add an implementation for an op name.
// The op set is closed in the runtime, so a permissive validator cannot make an
// unknown op work, only stop the abort: start_task() re-checks the shape and
// fails the task with -EINVAL. tests/t_pragma_custom_validator.c covers that.
typedef bool (*filc_async_validator_fn)(const filc_async_meta* meta);
void  filc_async_set_validator(filc_async_validator_fn fn);
void  filc_async_validate_table(const filc_async_meta* const* metas);
void  filc_async_fatal(const char* msg) __attribute__((noreturn));