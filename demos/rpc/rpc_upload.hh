#pragma once

/* The annotation layer of demo_rpc_upload: two runtimes in one program.
 *
 * async_pread, from pragma_io.hh, runs on runtime=io_uring: it fills `buf`
 * and marks it pending until the read lands. upload() below runs on
 * runtime=rpc (runtime/rpc/rpc_runtime.c):
 *
 *   runtime=rpc  the runtime that runs the call
 *   op=put       send `len` bytes of `data` to the server; the reply is
 *                their checksum
 *   const data   `data` points to const, so the call only reads it: it is
 *                not marked pending, but a later call that writes it waits
 *                for the upload. If a read is still filling it, the runtime
 *                holds the bytes back until that read lands.
 *
 * The runtime runs the body just before it sends the call; here it does
 * nothing, and the server does the work. */

#include "../pragma/pragma_io.hh"

FILC_ASYNC(rpc, FILC_OP(put))
void* upload(int port, const void* data, size_t len) {
  return 0;
}
