#pragma once

/* Setup, output and checks for demo_rpc_counter, so the demo's source holds
 * only the calls it demonstrates. */

#include <stdlib.h>

#include "../common/demo_checks.hh"
#include "rpc_counter.hh"

/* Returns the server's port, from the command line. */
static inline unsigned rpc_setup(int argc, char** argv) {
  unsigned long port = argc > 1 ? strtoul(argv[1], NULL, 10) : 0;
  if (port == 0 || port > 65535) {
    fprintf(stderr, "usage: demo_rpc_counter PORT\n");
    exit(2);
  }
  pragma_title("rpc: annotated calls on the program's own runtime",
               "  step() and get() are plain C; their pragmas say runtime=rpc.");
  return (unsigned)port;
}

static inline int rpc_report(const long* reply) {
  static const char* call[4] = {"get()", "get()", "step()", "get()"};
  filc_async_stats stats;
  filc_async_get_stats(&stats);

  printf("  %-8s %s\n", "call", "reply");
  for (int i = 0; i < 4; i++)
    printf("  %-8s %ld\n", call[i], reply[i]);
  printf("\n  => %lu calls answered by the server; %lu waited for a lock\n",
         stats.tasks_completed, stats.lock_waits);

  pragma_check("the gets before the step read 0", reply[0] == 0 && reply[1] == 0);
  pragma_check("the step made it 1, and the get after it read 1",
               reply[2] == 1 && reply[3] == 1);
  pragma_check("only the step and the get after it waited for a lock",
               stats.lock_waits == 2);
  pragma_check("every call completed without failing",
               stats.tasks_submitted == 4 && stats.tasks_completed == 4 &&
                   stats.tasks_failed == 0);
  return pragma_finish();
}
