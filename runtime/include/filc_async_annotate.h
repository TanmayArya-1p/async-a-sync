#pragma once

/* Macros that annotate a function as an asynchronous call.
 *
 *     FILC_ASYNC(io_uring, FILC_OP(pread), FILC_BOUT(buf), FILC_R_DEP(fd, file))
 *     void* read_at(int fd, void* buf, size_t len, unsigned long offset);
 *
 * FILC_ASYNC(runtime, options...) goes before the declaration or definition it
 * annotates. It names the runtime that runs the call and lists the options
 * that follow. Each option is its own macro, so a misspelt one is a compile
 * error instead of a string the compiler passes on unread:
 *
 *   FILC_OP(op)             the operation the runtime performs
 *   FILC_BIN(param)         the call reads the buffer param (never pending)
 *   FILC_BOUT(param)        the call fills the buffer param (pending until done)
 *   FILC_BUF(param)         direction unknown, treated as written
 *   FILC_R_DEP(param, ns)   the call reads the resource param names, in ns
 *   FILC_W_DEP(param, ns)   the call writes it
 *   FILC_OPTION(key, value) an option only the runtime reads
 *
 * Arguments are plain names, not strings: FILC_BOUT(buf), not FILC_BOUT("buf").
 * They name parameters, never positions. See wiki/Annotation-Reference.md for
 * what each option means.
 *
 * The macros expand to the attribute
 *
 *     __attribute__((annotate("filc_async", "runtime=io_uring", "op=pread", ...)))
 *
 * which is what the FilAsync pass reads. A function takes one FILC_ASYNC: the
 * pass keeps one annotation per function, so put every option inside it. To
 * share options between functions, name the whole annotation:
 *
 *     #define ASYNC_READ FILC_ASYNC(io_uring, FILC_OP(pread), FILC_BOUT(buf))
 *     ASYNC_READ void* read_a(int fd, void* buf, size_t len, unsigned long off);
 *     ASYNC_READ void* read_b(int fd, void* buf, size_t len, unsigned long off);
 *
 * The older `#pragma clang attribute push(__attribute__((annotate(...))),
 * apply_to=function)` form still works and means the same.
 *
 * The first argument of FILC_ASYNC and the arguments of the option macros are
 * stringized, so a name that is also a macro is not expanded. */

#define FILC_ASYNC(runtime, ...) \
    __attribute__((annotate("filc_async", "runtime=" #runtime \
                            __VA_OPT__(,) __VA_ARGS__)))

#define FILC_OP(op) "op=" #op
#define FILC_BIN(param) "bin=" #param
#define FILC_BOUT(param) "bout=" #param
#define FILC_BUF(param) "buf=" #param
#define FILC_R_DEP(param, ns) "r_dep=" #param ":" #ns
#define FILC_W_DEP(param, ns) "w_dep=" #param ":" #ns
#define FILC_OPTION(key, value) #key "=" #value
