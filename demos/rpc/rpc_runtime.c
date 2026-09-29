/* rpc_runtime.c: runtime=rpc, a runtime that turns each annotated call into
 * one request to the counter server in rpc_counter_server.c.
 *
 * submit connects to the server and sends the call's command, STEP or GET.
 * poll reads the reply, "VALUE <n>\n", and completes the call with n. The
 * server closes the connection after its reply, so the end of the stream is
 * the end of the reply. Nothing blocks except poll in BLOCK mode, which
 * waits for the socket.
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

struct request {
  pthread_mutex_t lock; /* held by the thread advancing the request */
  int fd;
  const char* command;
  size_t sent;
  char reply[32];
  size_t received;
  int done;
};

/* The command for the function's op=, or NULL for an op the server lacks. */
static const char* command_for(const filc_async_meta* meta) {
  for (const char* const* opt = meta->opts; *opt; opt++) {
    if (strcmp(*opt, "op=step") == 0)
      return "STEP\n";
    if (strcmp(*opt, "op=get") == 0)
      return "GET\n";
  }
  return NULL;
}

/* A function on this runtime names a command and takes one integer, the
 * server's port. */
static bool rpc_validate(const filc_async_meta* meta) {
  return command_for(meta) && meta->nargs == 1 &&
         meta->args[0].kind == FILC_ASYNC_ARG_IGNORED;
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

/* Sends what is left of the command and reads what has arrived of the
 * reply. Returns the socket event to wait for, or 0 once the call has
 * completed. */
static short advance(void* task, struct request* r) {
  size_t len = strlen(r->command);
  while (r->sent < len) {
    ssize_t n = send(r->fd, r->command + r->sent, len - r->sent,
                     MSG_DONTWAIT | MSG_NOSIGNAL);
    if (n < 0 && errno == EAGAIN) /* still connecting, or the buffer is full */
      return POLLOUT;
    if (n < 0) {
      finish(task, r, -errno);
      return 0;
    }
    r->sent += n;
  }
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
  /* The port, from the low word of the first staged argument cell. */
  uint16_t port = (uint16_t)*(uint64_t*)staged_args;

  struct request* r = calloc(1, sizeof(*r));
  pthread_mutex_init(&r->lock, NULL);
  r->command = command_for(meta);
  r->fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
  struct sockaddr_in addr = {0};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(port);
  if (connect(r->fd, (struct sockaddr*)&addr, sizeof(addr)) < 0 &&
      errno != EINPROGRESS) {
    finish(task, r, -errno);
    return;
  }

  /* Publish the request to poll, then send the command if the socket
   * already takes it. */
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

  short event = r->done || mode == FILC_ASYNC_POLL_CHECK ? 0 : advance(task, r);
  while (event && mode == FILC_ASYNC_POLL_BLOCK) {
    struct pollfd p = {r->fd, event, 0};
    poll(&p, 1, -1);
    event = advance(task, r);
  }
  bool done = r->done;
  pthread_mutex_unlock(&r->lock);
  return done;
}

FILC_ASYNC_RUNTIME(rpc, rpc_submit, rpc_poll, rpc_validate);
