#pragma once

// Logging, setup, timeline and checks for demo_rpc_counter, so the demo
// holds only its functions and calls.

#include <stdlib.h>
#include <time.h>

#include "../common/demo_checks.hh"
#include "filc_async.h"

#define RPC_REPLY_MS 50 // server delay per reply
#define RPC_CALLS 4

// Each call's op and send time, logged by the bodies.
#define RPC_MAX_CALLS 8
static double rpc_t0;
static const char* rpc_sent_op[RPC_MAX_CALLS];
static double rpc_sent_ms[RPC_MAX_CALLS];
static int rpc_nsent;

static inline double rpc_now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6 - rpc_t0;
}

// Called from a body, just before the call is sent.
static inline void rpc_log_sent(const char* op) {
  if (rpc_nsent < RPC_MAX_CALLS) {
    rpc_sent_op[rpc_nsent] = op;
    rpc_sent_ms[rpc_nsent] = rpc_now_ms();
  }
  rpc_nsent++;
}

// Port from the command line; starts the clock.
static inline unsigned rpc_setup(int argc, char** argv) {
  unsigned long port = argc > 1 ? strtoul(argv[1], NULL, 10) : 0;
  if (port == 0 || port > 65535) {
    fprintf(stderr, "usage: demo_rpc_counter PORT\n");
    exit(2);
  }
  pragma_title("rpc: calls to a TCP counter server that read like plain C",
               "  The server takes 50 ms per reply. No call is waited for:\n"
               "  reading a value waits for its reply.");
  rpc_t0 = rpc_now_ms();
  return (unsigned)port;
}

// The four values in call order, already read by main.
static inline int rpc_report(long first, long second, long stepped,
                             long after) {
  double read_ms = rpc_now_ms();
  long value[RPC_CALLS] = {first, second, stepped, after};
  filc_async_stats stats;
  filc_async_get_stats(&stats);

  printf("  %-6s %9s  %5s\n", "call", "sent at", "value");
  for (int i = 0; i < RPC_CALLS && i < rpc_nsent; i++)
    printf("  %-6s %6.0f ms  %5ld\n", rpc_sent_op[i], rpc_sent_ms[i],
           value[i]);
  printf("  %-6s %6.0f ms\n", "read", read_ms);

  // a call that waited was sent about one reply after the one before it
  int step_waited = rpc_sent_ms[2] - rpc_sent_ms[1] > 0.8 * RPC_REPLY_MS;
  int get_waited = rpc_sent_ms[3] - rpc_sent_ms[2] > 0.8 * RPC_REPLY_MS;
  printf("\n  => step %s for both gets, and the last get %s for the step.\n"
         "     4 calls in %.0f ms; one at a time they take %d ms.\n\n",
         step_waited ? "waited" : "did not wait",
         get_waited ? "waited" : "did not wait", read_ms,
         RPC_CALLS * RPC_REPLY_MS);

  pragma_check("the calls were sent as get, get, step, get",
               rpc_nsent == RPC_CALLS && strcmp(rpc_sent_op[2], "step") == 0);
  pragma_check("the gets before the step read 0", first == 0 && second == 0);
  pragma_check("the step made it 1, and the get after it read 1",
               stepped == 1 && after == 1);
  pragma_check("only the step and the get after it waited for a lock",
               stats.lock_waits == 2);
  pragma_check("the two gets ran together: step was sent within one reply",
               rpc_sent_ms[2] < 1.8 * RPC_REPLY_MS);
  pragma_check("reading the values waited for the last reply",
               read_ms - rpc_sent_ms[3] > 0.8 * RPC_REPLY_MS);
  pragma_check("every call completed without failing",
               stats.tasks_submitted == 4 && stats.tasks_completed == 4 &&
                   stats.tasks_failed == 0);
  return pragma_finish();
}
