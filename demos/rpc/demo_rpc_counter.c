/* demo_rpc_counter: calls to a TCP counter server that read like plain C.
 *
 * Each call returns before the server replies.
 * No handles, no waits: reading a value waits for its reply.
 *
 *   runtime=rpc         sent to the server by rpc_runtime.c
 *   op=step             add one to the counter
 *   op=get              read the counter
 *   w_dep=port:counter  writes the counter behind the port
 *   r_dep=port:counter  reads it
 *   bout=value          reply lands in *value, pending until it arrives
 *
 * Logging, timeline and checks: rpc_counter_report.hh. */

#include "rpc_counter_report.hh"

// Bodies run just before the call is sent; they only log.

#pragma clang attribute push(__attribute__((annotate("filc_async", "runtime=rpc", "op=step", "w_dep=port:counter", "bout=value"))), apply_to=function)
void step(unsigned port, long* value) {
  rpc_log_sent("step");
}
#pragma clang attribute pop

#pragma clang attribute push(__attribute__((annotate("filc_async", "runtime=rpc", "op=get", "r_dep=port:counter", "bout=value"))), apply_to=function)
void get(unsigned port, long* value) {
  rpc_log_sent("get");
}
#pragma clang attribute pop

int main(int argc, char** argv) {
  unsigned port = rpc_setup(argc, argv);
  long first, second, stepped, after;

  get(port, &first);
  get(port, &second);   // shares the read lock with the first
  step(port, &stepped); // waits here for both gets
  get(port, &after);    // waits here for the step

  // reading the values is the only wait
  return rpc_report(first, second, stepped, after);
}
