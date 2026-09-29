#pragma once

/* The annotation layer the demo_pragma_* programs share.
 *
 * Each function below is ordinary C under a `#pragma clang attribute` that
 * tells the patched compiler which syscall it stands for and what its
 * arguments are:
 *
 *   op=<syscall>  the io_uring operation a call becomes
 *   fd=<i>        argument i is the file descriptor
 *   bin=<i>       argument i is a buffer the kernel reads (never marked)
 *   bout=<i>      argument i is a buffer the kernel fills (marked pending
 *                 until the read lands; the first access waits for it)
 *   r_dep=<i>     argument i is a resource this call reads
 *   w_dep=<i>     argument i is a resource this call writes
 *
 * Calls that share a dependency key run in program order unless both only
 * read it. Here the key is the fd: a write, fsync or close waits for earlier
 * calls on the same fd, while reads of one fd may overlap each other.
 *
 * The FilAsync pass rewrites every call into filc_async_submit, which queues
 * an io_uring request and returns a task handle, so the bodies never run.
 * They count themselves in pragma_body_calls so a demo can show it is 0. */

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/resource.h>
#include <unistd.h>

#include "fasync.h"
#include "filc_async.h"
#include "utils.hh"

#ifndef FASYNC_COMPILER_INSERTS_CHECKS
#error "the pragma demos need the patched compiler: \
build with -DFASYNC_COMPILER_INSERTS_CHECKS"
#endif

static volatile int pragma_body_calls;

#pragma clang attribute push(__attribute__((annotate("filc_async", "op=openat", "fd=0", "bin=1"))), apply_to=function)
void* async_openat(int dirfd, const char* path, int flags, int mode) {
  pragma_body_calls++;
  return 0;
}
#pragma clang attribute pop

#pragma clang attribute push(__attribute__((annotate("filc_async", "op=pread", "fd=0", "bout=1", "r_dep=0", "w_dep=1"))), apply_to=function)
void* async_pread(int fd, void* buf, size_t len, unsigned long offset) {
  pragma_body_calls++;
  return 0;
}
#pragma clang attribute pop

#pragma clang attribute push(__attribute__((annotate("filc_async", "op=pwrite", "fd=0", "bin=1", "w_dep=0"))), apply_to=function)
void* async_pwrite(int fd, const void* buf, size_t len, unsigned long offset) {
  pragma_body_calls++;
  return 0;
}
#pragma clang attribute pop

#pragma clang attribute push(__attribute__((annotate("filc_async", "op=fsync", "fd=0", "w_dep=0"))), apply_to=function)
void* async_fsync(int fd) {
  pragma_body_calls++;
  return 0;
}
#pragma clang attribute pop

#pragma clang attribute push(__attribute__((annotate("filc_async", "op=close", "fd=0", "w_dep=0"))), apply_to=function)
void* async_close(int fd) {
  pragma_body_calls++;
  return 0;
}
#pragma clang attribute pop

/* Waits for an annotated call and returns its syscall result (-errno on
 * failure). */
static inline long pragma_wait(void* task) {
  struct filc_async_result_s r = {0};
  r.pending = task;
  filc_async_wait(&r);
  return r.result;
}

/* Counters from both layers plus the calling thread's context switches, so a
 * demo can report what one phase cost by subtracting two snapshots. */
struct pragma_snap {
  unsigned long calls;        /* annotated calls submitted */
  unsigned long completed;    /* annotated calls completed */
  unsigned long failed;       /* annotated calls that returned -errno */
  unsigned long sqes;         /* io_uring requests queued */
  unsigned long submits;      /* io_uring_enter calls that submitted */
  unsigned long waits;        /* io_uring_enter calls that slept */
  unsigned long hook_resolves; /* accesses the compiler's hook had to check */
  unsigned long sleeps;       /* voluntary context switches of this thread */
};

static inline void pragma_snap(struct pragma_snap* s) {
  filc_async_stats a;
  struct fasync_stats f;
  struct rusage ru;
  filc_async_get_stats(&a);
  fasync_get_stats(&f);
  getrusage(RUSAGE_THREAD, &ru);
  s->calls = a.tasks_submitted;
  s->completed = a.tasks_completed;
  s->failed = a.tasks_failed;
  s->sqes = f.sqes_queued;
  s->submits = f.kernel_submit_entries;
  s->waits = f.kernel_wait_entries;
  s->hook_resolves = f.resolve_calls;
  s->sleeps = (unsigned long)ru.ru_nvcsw;
}

static inline struct pragma_snap pragma_since(const struct pragma_snap* then) {
  struct pragma_snap now;
  pragma_snap(&now);
  now.calls -= then->calls;
  now.completed -= then->completed;
  now.failed -= then->failed;
  now.sqes -= then->sqes;
  now.submits -= then->submits;
  now.waits -= then->waits;
  now.hook_resolves -= then->hook_resolves;
  now.sleeps -= then->sleeps;
  return now;
}

/* Checks are recorded as a demo runs. pragma_finish prints one line for all
 * of them and names only the ones that failed, so a passing run stays short. */
#define PRAGMA_MAX_CHECKS 32
static const char* pragma_check_what[PRAGMA_MAX_CHECKS];
static int pragma_check_ok[PRAGMA_MAX_CHECKS];
static int pragma_nchecks;
static int pragma_failures;

static inline void pragma_check(const char* what, int ok) {
  if (pragma_nchecks < PRAGMA_MAX_CHECKS) {
    pragma_check_what[pragma_nchecks] = what;
    pragma_check_ok[pragma_nchecks] = ok;
    pragma_nchecks++;
  }
  if (!ok)
    pragma_failures++;
}

static inline void pragma_title(const char* title, const char* subtitle) {
  printf("\n%s\n", title);
  for (size_t i = 0; i < strlen(title); i++)
    putchar('-');
  printf("\n");
  if (subtitle)
    printf("%s\n\n", subtitle);
}

static inline int pragma_finish(void) {
  printf("  checks: %d/%d passed\n", pragma_nchecks - pragma_failures,
         pragma_nchecks);
  for (int i = 0; i < pragma_nchecks; i++)
    if (!pragma_check_ok[i])
      printf("    FAILED: %s\n", pragma_check_what[i]);
  printf("\n%s\n", pragma_failures ? "DEMO FAILED" : "DEMO OK");
  return pragma_failures ? 1 : 0;
}
