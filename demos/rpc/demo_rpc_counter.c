/* demo_rpc_counter: annotated calls on a runtime the program brings itself.
 *
 * step() and get() in rpc_counter.hh look like plain functions of a port.
 * Their pragmas say runtime=rpc, so rpc_runtime.c sends each call to the
 * counter server (rpc_counter_server.c) and the call returns at once. The
 * server's reply is the call's result.
 *
 * rpc_report.hh reads the port, prints the replies and checks them. */

#include "rpc_report.hh"

int main(int argc, char** argv) {
  unsigned port = rpc_setup(argc, argv);

  void* first = get(port);
  void* second = get(port);  /* shares the read lock: in flight with first */
  void* stepped = step(port); /* waits for both gets */
  void* after = get(port);   /* waits for the step */

  long reply[4] = {rpc_wait(first), rpc_wait(second), rpc_wait(stepped),
                   rpc_wait(after)};
  return rpc_report(reply);
}
