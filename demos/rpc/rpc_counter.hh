#pragma once

/* The annotation layer of demo_rpc_counter.
 *
 * step() and get() are ordinary C under a `#pragma clang attribute` that
 * hands every call to the runtime in rpc_runtime.c:
 *
 *   runtime=rpc  the runtime that runs the call: it sends the call to the
 *                counter server, and the server's reply is the call's result
 *   op=step      add one to the counter; the reply is the new value
 *   op=get       read the counter
 *   w_dep=port:counter  the call writes the counter behind the port
 *   r_dep=port:counter  the call reads it
 *
 * A write lock is exclusive and a read lock is shared: two gets run
 * together, and a step waits, inside the call, for the calls before it.
 * The runtime never runs the bodies; the server does the work. */

#include "filc_async.h"

#pragma clang attribute push(__attribute__((annotate("filc_async", "runtime=rpc", "op=step", "w_dep=port:counter"))), apply_to=function)
void* step(unsigned port) {
  return 0;
}
#pragma clang attribute pop

#pragma clang attribute push(__attribute__((annotate("filc_async", "runtime=rpc", "op=get", "r_dep=port:counter"))), apply_to=function)
void* get(unsigned port) {
  return 0;
}
#pragma clang attribute pop

/* Waits for an annotated call and returns the server's reply (-errno on
 * failure). */
static inline long rpc_wait(void* task) {
  struct filc_async_result_s r = {0};
  r.pending = task;
  filc_async_wait(&r);
  return r.result;
}
