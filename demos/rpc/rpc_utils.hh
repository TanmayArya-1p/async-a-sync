#pragma once

// What the rpc demos share: the call log and waiting. Each demo's own
// setup and report are in <demo>_utils.hh, so each demo file holds only its
// annotated functions and their calls.

#include "filc_async.h"
#include "../common/utils.hh"

// ---- the call log ----
// Each annotated body calls log_call. The runtime runs a body just before
// it sends the call.

#define LOG_MAX 16
static struct {
  int n;
  double t0;
  const char* name[LOG_MAX];
  double ms[LOG_MAX]; // since setup
} calls __attribute__((unused));

static inline void log_call(const char* name) {
  if (calls.n < LOG_MAX) {
    calls.name[calls.n] = name;
    calls.ms[calls.n] = now_ms() - calls.t0;
  }
  calls.n++;
}

// Waits for an annotated call; returns its result (-errno on failure).
static inline long wait_for(void* task) {
  struct filc_async_result_s r = {0};
  r.pending = task;
  filc_async_wait(&r);
  return r.result;
}

static inline unsigned port_arg(int argc, char** argv, const char* usage) {
  unsigned long port = argc > 1 ? strtoul(argv[1], NULL, 10) : 0;
  if (port == 0 || port > 65535) {
    fprintf(stderr, "usage: %s\n", usage);
    exit(2);
  }
  return (unsigned)port;
}
