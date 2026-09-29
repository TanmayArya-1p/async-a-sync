/* rpc_server: the server the rpc demos' calls go to.
 *
 * It listens on a free loopback port and prints "PORT <n>". Each connection
 * carries one command and gets one reply, "VALUE <n>\n", before the server
 * closes it:
 *
 *   STEP\n                 add one to the counter; n is the new value
 *   GET\n                  n is the counter
 *   PUT <len>\n<len bytes>  n is the FNV-1a checksum of the bytes
 *
 * Each reply takes 50 ms, so a call is still running after it returns.
 *
 * Plain C, built with the host compiler. */

#include <arpa/inet.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* Reads the payload of a PUT and returns its checksum, or -1. */
static long put(int fd, size_t len) {
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

  long counter = 0;
  for (;;) {
    int fd = accept(listener, NULL, NULL);
    if (fd < 0)
      continue;
    char command[32] = {0};
    size_t used = 0;
    while (used < sizeof(command) - 1 && !strchr(command, '\n') &&
           recv(fd, command + used, 1, 0) == 1)
      used++;

    long value = -1;
    size_t put_len;
    if (strcmp(command, "STEP\n") == 0)
      value = ++counter;
    else if (strcmp(command, "GET\n") == 0)
      value = counter;
    else if (sscanf(command, "PUT %zu\n", &put_len) == 1)
      value = put(fd, put_len);
    usleep(50000);

    char reply[32];
    int n = value >= 0 ? snprintf(reply, sizeof(reply), "VALUE %ld\n", value)
                       : snprintf(reply, sizeof(reply), "ERR\n");
    send(fd, reply, n, MSG_NOSIGNAL);
    close(fd);
  }
}
