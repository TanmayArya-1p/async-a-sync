# The RPC runtime example

**Source:** [`demos/rpc/rpc_runtime.c`](../demos/rpc/rpc_runtime.c)
**Programs:** [`demos/rpc/demo_rpc_counter.c`](../demos/rpc/demo_rpc_counter.c) and
[`demos/rpc/demo_rpc_upload.c`](../demos/rpc/demo_rpc_upload.c), with the
server [`demos/rpc/rpc_server.c`](../demos/rpc/rpc_server.c)
**Run:** `make demo-rpc` (both), `make demo-rpc-counter`, `make demo-rpc-upload`,
or `demos/rpc/run_rpc_demo.sh counter|upload [OUT_DIR]`

`runtime=rpc` is a runtime that belongs to a program, not to the repository's
libraries. It turns annotated calls into requests to a TCP server, so an
ordinary-looking `step(port)` call returns at once while the server does the
work. It is a complete example of the [Runtime API](Runtime-API.md) for a
runtime that talks to something other than the kernel's I/O queues: about 200
lines, with no change to the framework.

Two demos use it:

- **counter**: `runtime=rpc` alone, with calls ordered by dependency locks;
- **upload**: `runtime=rpc` next to `runtime=io_uring`. The loop reads files
  with io_uring and uploads each buffer over rpc.

## Run the counter demo

```sh
make demo-rpc-counter
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

## Run the upload demo

```sh
make demo-rpc-upload
```

```text
upload: read files with io_uring, upload them with rpc
------------------------------------------------------
  async_pread() says runtime=io_uring; upload() says runtime=rpc.

  file   bytes  server checksum    file checksum
  0      4096   0x64fb25ad         0x64fb25ad
  1      4096   0xbce74624         0xbce74624
  2      4096   0x9dfe3c82         0x9dfe3c82
  3      4096   0xfce16725         0xfce16725

  the loop returned in 1.00 ms; the last upload was answered after 202.5 ms
  the 4 reads reached the kernel in 1 submit

  => each upload waited for its own read, and the reads still went out as one batch
  checks: 3/3 passed

DEMO OK
```

This program links both runtimes:
`demo_rpc_upload.c rpc_runtime.c -lfilc_async_uring -lpizlo -lc`.

## The protocol

Each connection carries one command and one reply, then the server closes
it. The server keeps one counter, starting at zero:

| Command | Server action | Reply |
|---|---|---|
| `STEP\n` | adds one to the counter | `VALUE <new value>\n` |
| `GET\n` | reads the counter | `VALUE <value>\n` |
| `PUT <len>\n` then `<len>` bytes | receives the bytes | `VALUE <FNV-1a checksum of the bytes>\n` |

The server handles one connection at a time and takes 50 ms per reply, so a
call is still running after it returns. The protocol is only for the demo: it
has no framing, retries, or authentication.

## The counter program

The annotations, from `rpc_counter.hh`:

```c
#pragma clang attribute push(__attribute__((annotate("filc_async", "runtime=rpc", "op=step", "w_dep=port:counter"))), apply_to=function)
void* step(unsigned port) { return 0; }
#pragma clang attribute pop

#pragma clang attribute push(__attribute__((annotate("filc_async", "runtime=rpc", "op=get", "r_dep=port:counter"))), apply_to=function)
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
  as `port:counter`. `get` takes a read lock and `step` a write lock, so the
  two `get` calls are in flight together, and the `step` and the last `get`
  each wait inside the call for the calls before them. The demo checks that
  exactly those two calls waited for a lock.
- **Bodies.** The runtime never runs a body, since the server does the work.

## The upload program

`upload()`, from `rpc_upload.hh`, sends a buffer and returns its checksum:

```c
#pragma clang attribute push(__attribute__((annotate("filc_async", "runtime=rpc", "op=put", "bin=data"))), apply_to=function)
void* upload(unsigned port, const void* data, size_t len) { return 0; }
#pragma clang attribute pop
```

The loop, from `demo_rpc_upload.c`. `async_pread` is the io_uring read from
the pragma demos:

```c
for (int i = 0; i < UPLOAD_FILES; i++) {
  async_pread(u.fd[i], u.buf[i], UPLOAD_BYTES, 0);
  sent[i] = upload(u.port, u.buf[i], UPLOAD_BYTES);
}
```

What happens:

1. **The loop never waits.** Each `async_pread` only queues a request and
   marks `buf[i]` pending. Each `upload` connects and sends its `PUT` header,
   but holds the bytes back: `buf[i]` is still pending.
2. **Waiting for the first upload drives the reads.** The rpc runtime waits
   for its payload with `filc_async_wait_buffer`. That polls the read through
   the io_uring runtime, which sends all four queued reads to the kernel in
   one submit.
3. **Each upload sends its bytes as soon as its read lands.** The server
   replies with each file's checksum, and the demo checks it against the
   bytes it wrote.

The two runtimes never talk to each other. The pending buffer is the only
link between them, and the framework holds it. Without the wait, each upload
would send the buffer's initial zeros, and every checksum would be the same.

## How the runtime implements each function

### validate

`rpc_validate` accepts a function whose first argument is an unannotated
integer, the port, and whose `op=` is one of:

- `step` or `get`, with no other argument;
- `put`, followed by a `bin=` buffer and an unannotated length.

A function naming
`runtime=rpc` with any other `op=` stops the program before `main` with
`function cannot be registered on this runtime`.

### submit

`rpc_submit` starts the request and returns without waiting for it:

1. It builds the command from the op. For `put`, it also keeps the payload
   pointer and length from the second and third staged argument cells.
2. It reads the port from the first cell, opens a nonblocking socket, and
   starts `connect`.
3. It publishes the request in the task's
   [runtime data word](Runtime-API.md), then calls `advance`, which sends the
   command if the socket already takes it.

A connection that fails at once completes the call with `-errno`.

### poll

`advance` moves a request forward without blocking:

1. It sends what is left of the command.
2. For a `put`, it sends the payload, but only once
   `filc_async_is_pending(payload)` is false: a call may still be filling it.
3. It reads what has arrived of the reply. When the server closes the
   connection, it parses the reply and calls `filc_async_complete` with the
   number, or with `-errno`.

Until then it returns what it is waiting for: `POLLOUT` while connecting or
sending, `POLLIN` while reading, or `WAIT_PAYLOAD` while the payload is
pending.

| Mode | What `rpc_poll` does |
|---|---|
| `CHECK` | reports whether the request is done, without touching the socket |
| `PROGRESS` | calls `advance` once |
| `BLOCK` | calls `advance` until the request is done. In between, it waits in `poll(2)` for the socket event, or in `filc_async_wait_buffer` for the payload. |

**Waiting for a payload.** `filc_async_wait_buffer` waits for every other
call that owns the buffer, polling each through its own runtime. That is how
an rpc call drives an io_uring read without knowing that io_uring exists.

- **Before submit.** If the data word is still empty, submit has not finished.
  Poll returns `false`, and the framework waits for submit.
- **Threads.** Each request has a mutex. `BLOCK` takes it and holds it while
  it waits for the socket. `CHECK` and `PROGRESS` only try it, and return
  `false` while another thread is advancing the request.

## Adapting it

To call another service, keep the structure and replace the parts that are
specific to the counter:

- the ops in `op_of` and `rpc_submit`, and the checks in `rpc_validate`;
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
