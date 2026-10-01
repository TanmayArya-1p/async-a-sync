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
 *   bin=data     the call only reads `data`, so the call does not mark it.
 *                If a read is still filling it, the runtime holds the bytes
 *                back until that read lands.
 *
 * The runtime runs the body just before it sends the call; here it does
 * nothing, and the server does the work. */

#include "../pragma/pragma_io.hh"

FILC_ASYNC(rpc, FILC_OP(put), FILC_BIN(data))
void* upload(unsigned port, const void* data, size_t len) {
  return 0;
}
