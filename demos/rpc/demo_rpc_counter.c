/* demo_rpc_counter: calls to a TCP counter server that read like plain C.
 *
 * Each call returns before the server replies.
 * No handles, no waits: reading a value waits for its reply.
 *
 *   runtime=rpc         sent to the server by runtime/rpc/rpc_runtime.c
 *   op=step             add one to the counter
 *   op=get              read the counter
 *   w_dep=port:counter  writes the counter behind the port
 *   r_dep=port:counter  reads it
 *   bout=value          reply lands in *value, pending until it arrives
 *
 * Logging, timeline and checks: rpc_counter_report.hh. */

#include "rpc_counter_report.hh"

// Bodies run just before the call is sent; they only log.

FILC_ASYNC(rpc, FILC_OP(step), FILC_W_DEP(port, counter), FILC_BOUT(value))
void step(int port, long* value) {
  rpc_log_sent("step");
}

FILC_ASYNC(rpc, FILC_OP(get), FILC_R_DEP(port, counter), FILC_BOUT(value))
void get(int port, long* value) {
  rpc_log_sent("get");
}

int main(int argc, char** argv) {
  int port = rpc_setup(argc, argv);
  long first, second, stepped, after;

  get(port, &first);
  get(port, &second);   // shares the read lock with the first
  step(port, &stepped); // waits here for both gets
  get(port, &after);    // waits here for the step

  // reading the values is the only wait
  return rpc_report(first, second, stepped, after);
}
