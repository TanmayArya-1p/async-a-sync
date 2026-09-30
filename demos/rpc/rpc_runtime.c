/* rpc_runtime.c: runtime=rpc, one TCP request per annotated call.
 *
 * submit: run the body, connect, send STEP, GET or PUT and its payload.
 * poll:   read "VALUE <n>\n", store n in *value if the call has one,
 *         complete with n.
 * Only BLOCK mode blocks: on the socket, or on a PUT's pending payload.
 * Locks and pending marks are the framework's, taken before submit. */

#include <arpa/inet.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "filc_async_runtime.h"

// One 16-byte staged cell per argument: scalar in the low word, or a pointer.
typedef struct {
  union {
    void* ptr;
    uint64_t word;
  } value;
  uint64_t capability;
} staged_arg;

enum op { OP_NONE, OP_STEP, OP_GET, OP_PUT };

// Not a poll(2) event: the payload is still pending.
#define WAIT_PAYLOAD 0x4000

struct request {
  pthread_mutex_t lock; // held while advancing
  int fd;
  char command[32];
  size_t command_sent;
  const char* payload; // PUT only
  size_t payload_len;
  size_t payload_sent;
  char reply[32];
  size_t received;
  long* value; // the bout= argument of STEP and GET
  long result;
  int done;
};

static enum op op_of(const filc_async_meta* meta) {
  for (const char* const* opt = meta->opts; *opt; opt++) {
    if (strcmp(*opt, "op=step") == 0)
      return OP_STEP;
    if (strcmp(*opt, "op=get") == 0)
      return OP_GET;
    if (strcmp(*opt, "op=put") == 0)
      return OP_PUT;
  }
  return OP_NONE;
}

// Shape: the port first, then
//   step, get: a bout= reply pointer;
//   put:       a bin= payload and its length.
static bool rpc_validate(const filc_async_meta* meta) {
  if (meta->nargs < 1 || meta->args[0].kind != FILC_ASYNC_ARG_IGNORED)
    return false;
  switch (op_of(meta)) {
  case OP_STEP:
  case OP_GET:
    return meta->nargs == 2 && meta->args[1].kind == FILC_ASYNC_ARG_BUFFER_OUT;
  case OP_PUT:
    return meta->nargs == 3 &&
           meta->args[1].kind == FILC_ASYNC_ARG_BUFFER_IN &&
           meta->args[2].kind == FILC_ASYNC_ARG_IGNORED;
  default:
    return false;
  }
}

static long store_reply(void* request) {
  struct request* r = request;
  *r->value = r->result;
  return 0;
}

// Never freed: rpc_poll on another thread may still hold it.
static void finish(void* task, struct request* r, long result) {
  if (r->fd >= 0)
    close(r->fd);
  r->result = result;
  // store inside filc_async_run, or the hook waits on this very call
  if (r->value)
    filc_async_run(task, store_reply, r);
  r->done = 1;
  filc_async_complete(task, result);
}

static long parse_reply(const struct request* r) {
  long value;
  char end;
  if (sscanf(r->reply, "VALUE %ld%c", &value, &end) != 2 || end != '\n')
    return -EPROTO;
  return value;
}

// Send data[*sent..len): 0 when all sent, POLLOUT if the socket is still
// connecting or full, or -errno.
static int send_rest(int fd, const char* data, size_t len, size_t* sent) {
  while (*sent < len) {
    ssize_t n = send(fd, data + *sent, len - *sent, MSG_DONTWAIT | MSG_NOSIGNAL);
    if (n < 0)
      return errno == EAGAIN ? POLLOUT : -errno;
    *sent += n;
  }
  return 0;
}

// Move the request on; returns what to wait for, or 0 when done.
static int advance(void* task, struct request* r) {
  int rc = send_rest(r->fd, r->command, strlen(r->command), &r->command_sent);
  if (rc == 0 && r->payload_sent < r->payload_len) {
    // the payload is a bin= input another call may still be filling
    if (filc_async_is_pending(r->payload))
      return WAIT_PAYLOAD;
    rc = send_rest(r->fd, r->payload, r->payload_len, &r->payload_sent);
  }
  if (rc < 0) {
    finish(task, r, rc);
    return 0;
  }
  if (rc)
    return rc;
  for (;;) {
    ssize_t n = recv(r->fd, r->reply + r->received,
                     sizeof(r->reply) - 1 - r->received, MSG_DONTWAIT);
    if (n < 0 && errno == EAGAIN)
      return POLLIN;
    if (n <= 0) {
      finish(task, r, n < 0 ? -errno : parse_reply(r));
      return 0;
    }
    r->received += n;
  }
}

static void rpc_submit(void* task, const filc_async_meta* meta,
                       filc_async_run_fn run, void* staged_args, size_t nargs) {
  (void)nargs; // validated
  const staged_arg* args = staged_args;

  // the body runs before the call is sent; the server does the work
  filc_async_run(task, run, staged_args);

  struct request* r = calloc(1, sizeof(*r));
  if (!r)
    filc_async_fatal("rpc: out of memory");
  pthread_mutex_init(&r->lock, NULL);
  switch (op_of(meta)) {
  case OP_STEP:
    strcpy(r->command, "STEP\n");
    r->value = args[1].value.ptr;
    break;
  case OP_GET:
    strcpy(r->command, "GET\n");
    r->value = args[1].value.ptr;
    break;
  default:
    r->payload = args[1].value.ptr;
    r->payload_len = args[2].value.word;
    snprintf(r->command, sizeof(r->command), "PUT %zu\n", r->payload_len);
    break;
  }

  // publish first; poll waits on the lock until submit is done
  pthread_mutex_lock(&r->lock);
  __atomic_store_n(filc_async_task_runtime_data(task), r, __ATOMIC_RELEASE);

  struct sockaddr_in addr = {0};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons((uint16_t)args[0].value.word);
  r->fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
  if (r->fd < 0 || (connect(r->fd, (struct sockaddr*)&addr, sizeof(addr)) < 0 &&
                    errno != EINPROGRESS))
    finish(task, r, -errno);
  else
    advance(task, r); // send now if the socket is ready
  pthread_mutex_unlock(&r->lock);
}

static bool rpc_poll(void* task, enum filc_async_poll_mode mode) {
  struct request* r =
      __atomic_load_n(filc_async_task_runtime_data(task), __ATOMIC_ACQUIRE);
  if (!r) // submit not there yet
    return false;
  if (mode == FILC_ASYNC_POLL_BLOCK)
    pthread_mutex_lock(&r->lock);
  else if (pthread_mutex_trylock(&r->lock) != 0)
    return false; // another thread is advancing it

  int wait = r->done || mode == FILC_ASYNC_POLL_CHECK ? 0 : advance(task, r);
  while (wait && mode == FILC_ASYNC_POLL_BLOCK) {
    if (wait == WAIT_PAYLOAD) {
      // wait for the call filling the payload, through its own runtime
      filc_async_wait_buffer(task, r->payload);
    } else {
      struct pollfd p = {r->fd, (short)wait, 0};
      poll(&p, 1, -1);
    }
    wait = advance(task, r);
  }
  bool done = r->done;
  pthread_mutex_unlock(&r->lock);
  return done;
}

FILC_ASYNC_RUNTIME(rpc, rpc_submit, rpc_poll, rpc_validate);
