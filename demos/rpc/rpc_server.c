/* rpc_server: the server the rpc demos talk to.
 *
 * Prints "PORT <n>", then serves loopback connections.
 * One command per connection:
 *   "STEP\n"                    add one to the counter, reply the new value
 *   "GET\n"                     reply the counter
 *   "PUT <len>\n" + len bytes   reply the bytes' FNV-1a checksum
 * One reply, "VALUE <n>\n", then close.
 * One thread per connection, 50 ms per reply.
 * No ordering of its own: the client's locks do that.
 * Plain C, built with the host compiler. */

#include <arpa/inet.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

pthread_mutex_t counter_lock = PTHREAD_MUTEX_INITIALIZER;
long counter;

// Read a PUT's payload; its checksum, or -1.
long put(int fd, size_t len) {
  uint32_t hash = 2166136261u;
  unsigned char chunk[4096];
  while (len) {
    ssize_t n = recv(fd, chunk, len < sizeof(chunk) ? len : sizeof(chunk), 0);
    if (n <= 0)
      return -1;
    for (ssize_t i = 0; i < n; i++)
      hash = (hash ^ chunk[i]) * 16777619u;
    len -= n;
  }
  return hash;
}

void* serve(void* arg) {
  int fd = (int)(intptr_t)arg;
  char command[32] = {0};
  size_t used = 0;
  while (used < sizeof(command) - 1 && !strchr(command, '\n') &&
         recv(fd, command + used, 1, 0) == 1)
    used++;

  long value = -1;
  size_t len;
  if (sscanf(command, "PUT %zu\n", &len) == 1)
    value = put(fd, len);
  usleep(50000);
  pthread_mutex_lock(&counter_lock);
  if (strcmp(command, "STEP\n") == 0)
    value = ++counter;
  else if (strcmp(command, "GET\n") == 0)
    value = counter;
  pthread_mutex_unlock(&counter_lock);

  char reply[32];
  int n = value >= 0 ? snprintf(reply, sizeof(reply), "VALUE %ld\n", value)
                     : snprintf(reply, sizeof(reply), "ERR\n");
  send(fd, reply, n, MSG_NOSIGNAL);
  close(fd);
  return NULL;
}

int main(void) {
  int listener = socket(AF_INET, SOCK_STREAM, 0);
  struct sockaddr_in addr = {0};
  socklen_t len = sizeof(addr);
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (bind(listener, (struct sockaddr*)&addr, len) < 0 ||
      listen(listener, 64) < 0 ||
      getsockname(listener, (struct sockaddr*)&addr, &len) < 0) {
    perror("rpc_server");
    return 1;
  }
  printf("PORT %u\n", ntohs(addr.sin_port));
  fflush(stdout);

  for (;;) {
    int fd = accept(listener, NULL, NULL);
    if (fd < 0)
      continue;
    pthread_t thread;
    if (pthread_create(&thread, NULL, serve, (void*)(intptr_t)fd) != 0) {
      close(fd);
      continue;
    }
    pthread_detach(thread);
  }
}
