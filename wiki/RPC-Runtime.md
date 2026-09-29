# The RPC runtime example

**Source:** [`demos/rpc/rpc_runtime.c`](../demos/rpc/rpc_runtime.c)
**Program:** [`demos/rpc/demo_rpc_counter.c`](../demos/rpc/demo_rpc_counter.c), its
annotations in [`demos/rpc/rpc_counter.hh`](../demos/rpc/rpc_counter.hh), and the
server [`demos/rpc/rpc_counter_server.c`](../demos/rpc/rpc_counter_server.c)
**Run:** `make demo-rpc`, or `demos/rpc/run_rpc_counter.sh [OUT_DIR]`

`runtime=rpc` is a runtime that belongs to a program, not to the repository's
libraries. It turns annotated calls into requests to a TCP server, so an
ordinary-looking `step(port)` call returns at once while the server does the
work. It is a complete example of the [Runtime API](Runtime-API.md) for a
runtime that talks to something other than the kernel's I/O queues: about 150
lines, with no io_uring and no change to the framework.

## Run it

```sh
make demo-rpc
```

The script builds the server with the host compiler and the program with the
patched compiler, starts the server on a free loopback port, and runs the
program against it:

```text
rpc: annotated calls on the program's own runtime
-------------------------------------------------
  step() and get() are plain C; their pragmas say runtime=rpc.

  call     reply
  get()    0
  get()    0
  step()   1
  get()    1

  => 4 calls answered by the server; 2 waited for a lock
  checks: 4/4 passed

DEMO OK
```

The program links only this runtime: its link line is
`demo_rpc_counter.c rpc_runtime.c -lpizlo -lc`, with no
`-lfilc_async_uring`.

## The protocol

The server keeps one counter, starting at zero. Each connection carries one
command and one reply, then the server closes it:

| Command | Server action | Reply |
|---|---|---|
| `STEP\n` | adds one to the counter | `VALUE <new value>\n` |
| `GET\n` | reads the counter | `VALUE <value>\n` |

The server handles one connection at a time and takes 50 ms per reply, so a
call is still running after it returns. The protocol is only for the demo: it
has no framing, retries, or authentication.

## The program

The annotations, from `rpc_counter.hh`:

```c
#pragma clang attribute push(__attribute__((annotate("filc_async", "runtime=rpc", "op=step", "w_dep=0"))), apply_to=function)
void* step(unsigned port) { return 0; }
#pragma clang attribute pop

#pragma clang attribute push(__attribute__((annotate("filc_async", "runtime=rpc", "op=get", "r_dep=0"))), apply_to=function)
void* get(unsigned port) { return 0; }
#pragma clang attribute pop
```

The calls, from `demo_rpc_counter.c`:

```c
void* first = get(port);
void* second = get(port);   /* shares the read lock: in flight with first */
void* stepped = step(port); /* waits for both gets */
void* after = get(port);    /* waits for the step */
```

- **Results.** A call's result is the server's number. Read it with
  `filc_async_wait` or `filc_async_poll` on the handle the call returns.
- **Failures.** A refused connection completes the call with
  `-ECONNREFUSED`, a malformed reply with `-EPROTO`.
- **Ordering.** The runtime ignores dependencies; the program declares them,
  keyed by the port. `get` takes a read lock and `step` a write lock, so the
  two `get` calls are in flight together, and the `step` and the last `get`
  each wait inside the call for the calls before them. The demo checks that
  exactly those two calls waited for a lock.
- **Bodies.** The runtime never runs a body, since the server does the work.

## How the runtime implements each function

### validate

`rpc_validate` accepts a function whose `op=` is `step` or `get` and that
takes exactly one unannotated integer, the port. A function naming
`runtime=rpc` with any other `op=` stops the program before `main` with
`function cannot be registered on this runtime`.

### submit

`rpc_submit` starts the request and returns without waiting for it:

1. It reads the port from the low word of the first staged argument cell.
2. It opens a nonblocking socket and starts `connect`.
3. It publishes the request in the task's
   [runtime data word](Runtime-API.md), then calls `advance`, which sends the
   command if the socket already takes it.

A connection that fails at once completes the call with `-errno`.

### poll

`advance` moves a request forward without blocking. It sends what is left of
the command, then reads what has arrived of the reply. When the server closes
the connection, it parses the reply and calls `filc_async_complete` with the
number, or with `-errno`. Otherwise it returns the socket event it is waiting
for: `POLLOUT` while still connecting or sending, `POLLIN` while reading.

| Mode | What `rpc_poll` does |
|---|---|
| `CHECK` | reports whether the request is done, without touching the socket |
| `PROGRESS` | calls `advance` once |
| `BLOCK` | calls `advance`, waiting in `poll(2)` for the event it returns, until the request is done |

- **Before submit.** If the data word is still empty, submit has not finished.
  Poll returns `false`, and the framework waits for submit.
- **Threads.** Each request has a mutex. `BLOCK` takes it and holds it while
  it waits for the socket. `CHECK` and `PROGRESS` only try it, and return
  `false` while another thread is advancing the request.

## Adapting it

To call another service, keep the structure and replace the parts that are
specific to the counter:

- the commands in `command_for`, and the checks in `rpc_validate`;
- the reply parser in `parse_reply`;
- the address in `rpc_submit`.

The arguments come from the staged cells described in
[Use the arguments](Writing-a-Runtime.md#4-use-the-arguments).

**Replies in a buffer.** To return a reply in a buffer instead of a number,
declare the buffer with `bout=`, so accessing it waits for the call. The
runtime must not load or store that buffer itself: its own code is built
with the access hook, so it would wait for its own call. Have libc write it
instead, for example by passing the buffer straight to `recv`. Fil-C's libc
does not test the pending flag (see [Limitations](Limitations.md#pending-buffers)).

## See also

- [Write a runtime](Writing-a-Runtime.md): the runtime API step by step.
- [The io_uring runtime](io_uring-Runtime.md): the repository's own runtime.
- [Runtime API](Runtime-API.md): the full contract.
