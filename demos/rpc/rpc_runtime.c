/* rpc_runtime.c: runtime=rpc, a runtime that turns each annotated call into
 * one request to the server in rpc_server.c.
 *
 * submit connects to the server and sends the call's command: STEP, GET, or
 * PUT followed by a payload. poll reads the reply, "VALUE <n>\n", and
 * completes the call with n. The server closes the connection after its
 * reply, so the end of the stream is the end of the reply. Nothing blocks
 * except poll in BLOCK mode, which waits for the socket, or for the payload.
 *
 * A PUT's payload is a bin= argument: the call only reads it, but another
 * call, on this runtime or another, may still be filling it. The runtime
 * sends it only once no call owns it any more.
 *
 * The framework has already taken the call's locks before submit, so the
 * runtime never looks at r_dep or w_dep. */

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

/* One 16-byte cell per staged argument: a scalar in the low word, or a
 * pointer (its capability travels with it). */
typedef struct {
  union {
    void* ptr;
    uint64_t word;
  } value;
  uint64_t capability;
} staged_arg;

enum op { OP_NONE, OP_STEP, OP_GET, OP_PUT };

/* Not a poll(2) event: the payload is still pending. */
#define WAIT_PAYLOAD 0x4000

struct request {
  pthread_mutex_t lock; /* held by the thread advancing the request */
  int fd;
  char command[32];
  size_t command_sent;
  const char* payload; /* PUT only */
  size_t payload_len;
  size_t payload_sent;
  char reply[32];
  size_t received;
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

/* Every function on this runtime takes the server's port first, an integer.
 * op=step and op=get take nothing else; op=put takes the payload, bin=, and
 * its length. */
static bool rpc_validate(const filc_async_meta* meta) {
  if (meta->nargs < 1 || meta->args[0].kind != FILC_ASYNC_ARG_IGNORED)
    return false;
  switch (op_of(meta)) {
  case OP_STEP:
  case OP_GET:
    return meta->nargs == 1;
  case OP_PUT:
    return meta->nargs == 3 &&
           meta->args[1].kind == FILC_ASYNC_ARG_BUFFER_IN &&
           meta->args[2].kind == FILC_ASYNC_ARG_IGNORED;
  default:
    return false;
  }
}

static void finish(void* task, struct request* r, long result) {
  close(r->fd);
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

/* Sends data[*sent..len). Returns 0 once it is all sent, POLLOUT while the
 * socket is still connecting or full, or -errno. */
static int send_rest(int fd, const char* data, size_t len, size_t* sent) {
  while (*sent < len) {
    ssize_t n = send(fd, data + *sent, len - *sent, MSG_DONTWAIT | MSG_NOSIGNAL);
    if (n < 0)
      return errno == EAGAIN ? POLLOUT : -errno;
    *sent += n;
  }
  return 0;
}

/* Sends what is left of the command and the payload, and reads what has
 * arrived of the reply. Returns what to wait for, or 0 once the call has
 * completed. */
static int advance(void* task, struct request* r) {
  int rc = send_rest(r->fd, r->command, strlen(r->command), &r->command_sent);
  if (rc == 0 && r->payload_sent < r->payload_len) {
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
  const staged_arg* args = staged_args;
  struct request* r = calloc(1, sizeof(*r));
  pthread_mutex_init(&r->lock, NULL);
  switch (op_of(meta)) {
  case OP_STEP:
    strcpy(r->command, "STEP\n");
    break;
  case OP_GET:
    strcpy(r->command, "GET\n");
    break;
  default:
    r->payload = args[1].value.ptr;
    r->payload_len = args[2].value.word;
    snprintf(r->command, sizeof(r->command), "PUT %zu\n", r->payload_len);
    break;
  }

  r->fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
  struct sockaddr_in addr = {0};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons((uint16_t)args[0].value.word);
  if (connect(r->fd, (struct sockaddr*)&addr, sizeof(addr)) < 0 &&
      errno != EINPROGRESS) {
    finish(task, r, -errno);
    return;
  }

  /* Publish the request to poll, then send what the socket already takes. */
  pthread_mutex_lock(&r->lock);
  __atomic_store_n(filc_async_task_runtime_data(task), r, __ATOMIC_RELEASE);
  advance(task, r);
  pthread_mutex_unlock(&r->lock);
}

static bool rpc_poll(void* task, enum filc_async_poll_mode mode) {
  struct request* r =
      __atomic_load_n(filc_async_task_runtime_data(task), __ATOMIC_ACQUIRE);
  if (!r) /* submit has not got that far yet */
    return false;
  if (mode == FILC_ASYNC_POLL_BLOCK)
    pthread_mutex_lock(&r->lock);
  else if (pthread_mutex_trylock(&r->lock) != 0)
    return false; /* another thread is advancing it */

  int wait = r->done || mode == FILC_ASYNC_POLL_CHECK ? 0 : advance(task, r);
  while (wait && mode == FILC_ASYNC_POLL_BLOCK) {
    if (wait == WAIT_PAYLOAD) {
      /* Waits for the call filling the payload, through its own runtime. */
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
