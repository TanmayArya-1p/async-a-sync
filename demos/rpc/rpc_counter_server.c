/* rpc_counter_server: the server demo_rpc_counter's calls go to.
 *
 * It listens on a free loopback port and prints "PORT <n>". Each connection
 * carries one command, "STEP\n" to add one to the counter or "GET\n" to read
 * it, and gets one reply, "VALUE <counter>\n", before the server closes it.
 * Each reply takes 50 ms, so a call is still running after it returns.
 *
 * Plain C, built with the host compiler. */

#include <arpa/inet.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

int main(void) {
  int listener = socket(AF_INET, SOCK_STREAM, 0);
  struct sockaddr_in addr = {0};
  socklen_t len = sizeof(addr);
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (bind(listener, (struct sockaddr*)&addr, len) < 0 ||
      listen(listener, 16) < 0 ||
      getsockname(listener, (struct sockaddr*)&addr, &len) < 0) {
    perror("rpc_counter_server");
    return 1;
  }
  printf("PORT %u\n", ntohs(addr.sin_port));
  fflush(stdout);

  long counter = 0;
  for (;;) {
    int fd = accept(listener, NULL, NULL);
    if (fd < 0)
      continue;
    char command[16] = {0};
    size_t used = 0;
    while (used < sizeof(command) - 1 && !strchr(command, '\n') &&
           recv(fd, command + used, 1, 0) == 1)
      used++;

    int known = 1;
    if (strcmp(command, "STEP\n") == 0)
      counter++;
    else if (strcmp(command, "GET\n") != 0)
      known = 0;
    usleep(50000);

    char reply[32];
    int n = known ? snprintf(reply, sizeof(reply), "VALUE %ld\n", counter)
                  : snprintf(reply, sizeof(reply), "ERR\n");
    send(fd, reply, n, MSG_NOSIGNAL);
    close(fd);
  }
}
