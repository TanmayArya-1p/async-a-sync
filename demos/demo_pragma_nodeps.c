// demos/demo_pragma_nodeps.c -- the smallest possible pragma demo: one
// annotated call, no dependencies.
//
// The annotated body is dead code. The pass rewrites the call into a submit,
// the runtime reads op=pread off the annotation and queues a READ SQE. The
// punchline this demo shows: submitting costs no syscall. The SQE sits in
// userspace until someone actually needs the result.

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "filc_async.h"

#define SZ (16u << 10)

static int failures;
static volatile int body_calls;

#pragma clang attribute push(__attribute__((annotate("filc_async", "op=pread", "fd=0", "bout=1"))), apply_to=function)
__attribute__((noinline)) void* async_pread(int fd, void* buf, size_t len, unsigned long offset);
#pragma clang attribute pop

// Never called. The runtime runs the syscall itself; this exists only so the
// call site has a callee to rewrite.
__attribute__((noinline)) void* async_pread(int fd, void* buf, size_t len, unsigned long offset)
{
    body_calls++;
    return (void*)(long)len;
}

static void check(int cond, const char* what)
{
    printf("  %s  %s\n", cond ? "ok  " : "FAIL", what);
    if (!cond)
        failures++;
}

static long finish(void* task, unsigned char* state)
{
    struct filc_async_result_s r = { .pending = task };
    if (!filc_async_poll(&r))
        filc_async_wait(&r);
    *state = r.state;
    return r.result;
}

int main(int argc, char** argv)
{
    const char* dir = argc > 1 ? argv[1] : "/tmp";
    char path[512];
    snprintf(path, sizeof path, "%s/demo_pragma_nodeps_%ld.dat", dir, (long)getpid());

    int seed = open(path, O_CREAT | O_TRUNC | O_RDWR, 0600);
    if (seed < 0) {
        perror("open seed");
        return 2;
    }
    unsigned char* zeros = calloc(1, SZ);
    for (int i = 0; i < SZ; i++)
        zeros[i] = (unsigned char)('A' + (i % 26));
    if (write(seed, zeros, SZ) != (ssize_t)SZ) {
        perror("write seed");
        return 2;
    }
    close(seed);

    printf("pragma async: one call, no dependencies\n");

    int fd = open(path, O_RDONLY);
    check(fd >= 0, "opened the seed file");
    if (fd < 0)
        return 2;

    unsigned char* buf = malloc(SZ);
    memset(buf, 0x11, SZ); // poison, so "unwritten" is visible

    void* task = async_pread(fd, buf, SZ, 0);
    check(task != NULL, "submit returned a task");

    // The interesting bit: the SQE is written but not yet handed to the kernel.
    filc_async_stats st;
    filc_async_get_stats(&st);
    check(st.tasks_submitted == 1, "one task submitted");
    check(st.sqes_queued == 1, "one SQE queued");
    check(st.kernel_submit_entries == 0, "kernel has NOT been entered yet (lazy submit)");

    unsigned char state = 0;
    long res = finish(task, &state);
    check(state == 0 && res == (long)SZ, "task completed with the byte count");

    filc_async_get_stats(&st);
    check(st.kernel_submit_entries == 1, "now the kernel was entered exactly once");

    int bad = 0;
    for (size_t i = 0; i < SZ; i++)
        if (buf[i] != zeros[i]) { bad = 1; break; }
    check(!bad, "buffer holds the file bytes, not the 0x11 poison");
    check(body_calls == 0, "the annotated body never executed");

    filc_async_get_stats(&st);
    check(st.tasks_completed == 1 && st.tasks_failed == 0, "1 completed, 0 failed");

    printf("%s\n", failures ? "DEMO FAILED" : "DEMO OK");
    free(buf);
    free(zeros);
    close(fd);
    unlink(path);
    return failures ? 1 : 0;
}
