# The RPC runtime example

**Source:** [`runtime/rpc/rpc_runtime.c`](../runtime/rpc/rpc_runtime.c)
**Library:** `runtime/build/lib/libfilc_async_rpc.a`, built by `runtime/build.sh`
**Programs:** [`demos/rpc/demo_rpc_counter.c`](../demos/rpc/demo_rpc_counter.c), which
holds its annotated functions and their calls, and
[`demos/rpc/demo_rpc_upload.c`](../demos/rpc/demo_rpc_upload.c); their logging and
checks are in `counter_utils.hh` and `upload_utils.hh`, and the server is
[`demos/rpc/rpc_server.c`](../demos/rpc/rpc_server.c)
**Run:** `make demo-rpc` (both), `make demo-rpc-counter`, `make demo-rpc-upload`,
or `demos/rpc/run_rpc_demo.sh counter|upload [OUT_DIR]`

`runtime=rpc` is the repository's second runtime. It turns annotated calls
into requests to the rpc demos' TCP server. An
ordinary-looking `get(port, &value)` returns before the server replies, and
the first read of `value` waits for the reply: the program never holds a
handle or calls a wait. It is a complete example of the [Runtime API](Runtime-API.md) for a
runtime that talks to something other than the kernel's I/O queues: about 200
lines, with no change to the framework.

Two demos use it:

- **counter**: `runtime=rpc` alone, with calls ordered by dependency locks
  and replies read from `bout=` values;
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
rpc: calls to a TCP counter server that read like plain C
---------------------------------------------------------
  The server takes 50 ms per reply. No call is waited for:
  reading a value waits for its reply.

  call     sent at  value
  get         0 ms      0
  get         0 ms      0
  step       51 ms      1
  get       102 ms      1
  read      152 ms

  => step waited for both gets, and the last get waited for the step.
     4 calls in 152 ms; one at a time they take 200 ms.

  checks: 7/7 passed

DEMO OK
```

- **sent at** is when the runtime sent each call, logged by the call's body.
  The two gets go out together; the step goes out once both replies are in,
  and the last get once the step's reply is in.
- **read** is when `main` had read all four values. It is one reply after the
  last get was sent: reading `after` waited for the server.

The program links only this runtime: its link line is
`demo_rpc_counter.c -lfilc_async_rpc -lpizlo -lc`, with no
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

  the loop's last call ran at 0.90 ms; the last upload was answered after 202.4 ms
  the 4 reads reached the kernel in 1 submit

  => each upload waited for its own read, and the reads still went out as one batch
  checks: 3/3 passed

DEMO OK
```

This program links both runtimes:
`demo_rpc_upload.c -lfilc_async_rpc -lfilc_async_uring -lpizlo -lc`.

## The protocol

Each connection carries one command and one reply, then the server closes
it. The server keeps one counter, starting at zero:

| Command | Server action | Reply |
|---|---|---|
| `STEP\n` | adds one to the counter | `VALUE <new value>\n` |
| `GET\n` | reads the counter | `VALUE <value>\n` |
| `PUT <len>\n` then `<len>` bytes | receives the bytes | `VALUE <FNV-1a checksum of the bytes>\n` |

The server serves each connection on its own thread and takes 50 ms per
reply, so calls sent together are served together. It does not order them:
a `STEP` sent during a `GET` would race it. The client's `r_dep`/`w_dep`
locks keep them apart. The protocol is only for the demo: it has no framing,
retries, or authentication.

## The counter program

The annotated functions, from `demo_rpc_counter.c`:

```c
#pragma clang attribute push(__attribute__((annotate("filc_async", "runtime=rpc", "op=step", "w_dep=port:counter", "bout=value"))), apply_to=function)
void step(unsigned port, long* value) {
  log_call("step");
}
#pragma clang attribute pop

#pragma clang attribute push(__attribute__((annotate("filc_async", "runtime=rpc", "op=get", "r_dep=port:counter", "bout=value"))), apply_to=function)
void get(unsigned port, long* value) {
  log_call("get");
}
#pragma clang attribute pop
```

And their calls, in `main` below them:

```c
long first, second, stepped, after;

get(port, &first);
get(port, &second);   // shares the read lock with the first
step(port, &stepped); // waits here for both gets
get(port, &after);    // waits here for the step

// reading the values is the only wait
return report(first, second, stepped, after);
```

- **Results.** The server's number lands in `*value`. `bout=value` marks
  `value` pending until the reply arrives, so the first read of it waits.
- **Failures.** A failed call stores `-errno` in `*value`: `-ECONNREFUSED`
  for a refused connection, `-EPROTO` for a malformed reply. The counter is
  never negative.
- **Ordering.** The runtime ignores dependencies; the program declares them,
  as `port:counter`. `get` takes a read lock and `step` a write lock, so the
  two `get` calls are in flight together, and the `step` and the last `get`
  each wait inside the call for the calls before them. The demo checks that
  exactly those two calls waited for a lock.
- **Bodies.** The runtime runs each body just before it sends the call, as
  the io_uring runtime does. Here the bodies log the call for the timeline;
  the server does the work.

## The upload program

`demo_rpc_upload.c` defines one annotated function on each runtime:

```c
// pread on io_uring: the kernel fills buf.
#pragma clang attribute push(__attribute__((annotate("filc_async", "runtime=io_uring", "op=pread", "bout=buf"))), apply_to=function)
void* async_pread(int fd, void* buf, size_t len, unsigned long offset) {
  log_call("async_pread");
  return 0;
}
#pragma clang attribute pop

// put on rpc: sends len bytes of data; the reply is their checksum.
#pragma clang attribute push(__attribute__((annotate("filc_async", "runtime=rpc", "op=put", "bin=data"))), apply_to=function)
void* upload(unsigned port, const void* data, size_t len) {
  log_call("upload");
  return 0;
}
#pragma clang attribute pop
```

And the loop that calls them:

```c
for (int i = 0; i < UPLOAD_FILES; i++) {
  async_pread(d.fd[i], d.buf[i], UPLOAD_BYTES, 0);  // queues the read
  sent[i] = upload(d.port, d.buf[i], UPLOAD_BYTES); // sends once the read lands
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
3. **Each upload sends its bytes once its read has landed and the upload is
   waited on.** The runtime moves a request forward only when that request
   is polled, so the demo's waits send the uploads one after another. The
   server replies with each file's checksum, and the demo checks it against
   the bytes it wrote.

The two runtimes never talk to each other. The pending buffer is the only
link between them, and the framework holds it. Without the wait, each upload
would send the buffer's initial zeros, and every checksum would be the same.

## How the runtime implements each function

### validate

`rpc_validate` accepts a function whose first argument is an unannotated
integer, the port, and whose `op=` is one of:

- `step` or `get`, followed by a `bout=` pointer for the reply;
- `put`, followed by a `bin=` buffer and an unannotated length.

A function naming
`runtime=rpc` with any other shape stops the program before `main` with
`function cannot be registered on this runtime`.

### submit

`rpc_submit` starts the request and returns without waiting for it:

1. It runs the call's body with `filc_async_run`.
2. It builds the command from the op. For `step` and `get` it keeps the
   reply pointer from the second staged argument cell; for `put`, the
   payload pointer and length from the second and third.
3. It publishes the request in the task's
   [runtime data word](Runtime-API.md), reads the port from the first cell,
   opens a nonblocking socket and starts `connect`, then calls `advance`,
   which sends the command if the socket already takes it.

A connection that fails at once completes the call with `-errno`.

### poll

`advance` moves a request forward without blocking:

1. It sends what is left of the command.
2. For a `put`, it sends the payload, but only once
   `filc_async_is_pending(payload)` is false: a call may still be filling it.
3. It reads what has arrived of the reply. When the server closes the
   connection, it parses the reply, stores it in `*value` for `step` and
   `get`, and calls `filc_async_complete` with the number, or with `-errno`.

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
  `false` while another thread is advancing it.
- **Requests are not freed.** Another thread may still be waiting for a
  request's mutex in `rpc_poll` after it completes, so there is no safe point
  to free it without a reference count. The demos make a handful of calls.

### Storing the reply

`*value` is the call's own pending buffer. Code built with the access hook
(the patched compiler) that did a plain `*value = n` would wait for the call
itself and never return. `runtime/build.sh` builds the runtime with the stock
`filcc`, which has no hook, but the store is written to be correct either
way, so the file can also be compiled into a program. The runtime makes the
store inside
`filc_async_run(task, store_reply, r)`: while `filc_async_run` runs, the
task may use its own pending buffers without waiting. It then calls
`filc_async_complete`, which resolves the buffer for everyone else. The value
is in place before any reader can see the buffer resolved.

Another way is to have libc write the buffer, for example by passing it
straight to `recv`: Fil-C's libc does not test the pending flag (see
[Limitations](Limitations.md#pending-buffers)).

## Adapting it

To call another service, keep the structure and replace the parts that are
specific to the counter:

- the ops in `op_of` and `rpc_submit`, and the checks in `rpc_validate`;
- the reply parser in `parse_reply`, and what `store_reply` writes;
- the address in `rpc_submit`.

The arguments come from the staged cells described in
[Use the arguments](Writing-a-Runtime.md#4-use-the-arguments).

## See also

- [Write a runtime](Writing-a-Runtime.md): the runtime API step by step.
- [The io_uring runtime](io_uring-Runtime.md): the repository's own runtime.
- [Runtime API](Runtime-API.md): the full contract.
