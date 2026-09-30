#pragma once

// Setup and report for demo_rpc_counter.

#include "rpc_utils.hh"

#define REPLY_MS 50 // the server's delay per reply

static inline unsigned setup(int argc, char** argv) {
  unsigned port = port_arg(argc, argv, "demo_rpc_counter PORT");
  title("rpc: calls to a TCP counter server that read like plain C",
        "  The server takes 50 ms per reply. No call is waited for:\n"
        "  reading a value waits for its reply.");
  calls.t0 = now_ms();
  return port;
}

// The four values in call order, already read by main.
static inline int report(long first, long second, long stepped, long after) {
  double read_ms = now_ms() - calls.t0;
  long value[4] = {first, second, stepped, after};
  filc_async_stats stats;
  filc_async_get_stats(&stats);

  printf("  %-6s %9s  %5s\n", "call", "sent at", "value");
  for (int i = 0; i < 4 && i < calls.n; i++)
    printf("  %-6s %6.0f ms  %5ld\n", calls.name[i], calls.ms[i], value[i]);
  printf("  %-6s %6.0f ms\n", "read", read_ms);

  // A call that waited was sent about one reply after the one before it.
  int step_waited = calls.ms[2] - calls.ms[1] > 0.8 * REPLY_MS;
  int get_waited = calls.ms[3] - calls.ms[2] > 0.8 * REPLY_MS;
  printf("\n  => step %s for both gets, and the last get %s for the step.\n"
         "     4 calls in %.0f ms; one at a time they take %d ms.\n\n",
         step_waited ? "waited" : "did not wait",
         get_waited ? "waited" : "did not wait", read_ms, 4 * REPLY_MS);

  check("the calls were sent as get, get, step, get",
        calls.n == 4 && strcmp(calls.name[2], "step") == 0);
  check("the gets before the step read 0", first == 0 && second == 0);
  check("the step made it 1, and the get after it read 1",
        stepped == 1 && after == 1);
  check("only the step and the get after it waited for a lock",
        stats.lock_waits == 2);
  check("the two gets ran together: step was sent within one reply",
        calls.ms[2] < 1.8 * REPLY_MS);
  check("reading the values waited for the last reply",
        read_ms - calls.ms[3] > 0.8 * REPLY_MS);
  check("every call completed without failing",
        stats.tasks_submitted == 4 && stats.tasks_completed == 4 &&
            stats.tasks_failed == 0);
  return finish();
}
