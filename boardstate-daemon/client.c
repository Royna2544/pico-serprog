#include "client.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static int transact(const char *request) {
  const char *path = getenv("BOARDSTATE_SOCKET");
  if (!path || !*path) path = "/tmp/boardstate-daemon.sock";
  struct sockaddr_un address = {.sun_family = AF_UNIX};
  if (strlen(path) >= sizeof(address.sun_path)) {
    errno = ENAMETOOLONG;
    return -1;
  }
  strcpy(address.sun_path, path);
  int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) return -1;
  if (connect(fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
    int saved = errno;
    close(fd);
    errno = saved;
    return -1;
  }
  size_t length = strlen(request);
  if (send(fd, request, length, MSG_NOSIGNAL) != (ssize_t)length) {
    int saved = errno;
    close(fd);
    errno = saved ? saved : EIO;
    return -1;
  }
  char response[256];
  ssize_t n = recv(fd, response, sizeof(response) - 1, 0);
  int saved = errno;
  close(fd);
  if (n <= 0 || n >= (ssize_t)sizeof(response) - 1) {
    errno = n < 0 ? saved : EPROTO;
    return -1;
  }
  response[n] = 0;
  if (!strcmp(response, "OK\n")) return 0;
  if (!strncmp(response, "ERR", 3)) {
    if (response[n - 1] == '\n') response[n - 1] = 0;
    fprintf(stderr, "boardstate: %s (request: %s)\n", response, request);
    errno = EIO;
    return -1;
  }
  char *end;
  long value = strtol(response, &end, 10);
  if (end == response || strcmp(end, "\n") || value < -1 || value > INT_MAX) {
    errno = EPROTO;
    return -1;
  }
  return (int)value;
}

int board_gpio(const char *board, unsigned pin, const char *operation,
               const char *value) {
  char request[96];
  int n = snprintf(request, sizeof(request), "GPIO %s %u %s%s%s", board,
                   pin, operation, value ? " " : "", value ? value : "");
  if (n < 0 || n >= (int)sizeof(request)) { errno = EINVAL; return -1; }
  return transact(request);
}

int board_uart(const char *board, const char *operation, int byte) {
  char request[96];
  int n = byte < 0 ? snprintf(request, sizeof(request), "UART %s %s", board, operation)
                   : snprintf(request, sizeof(request), "UART %s %s %d", board, operation, byte);
  if (n < 0 || n >= (int)sizeof(request)) { errno = EINVAL; return -1; }
  return transact(request);
}

int board_power(void) { return transact("POWER esp"); }
int board_reset_pico(void) { return transact("RESET pico"); }
int board_irq_pending(void) { return transact("IRQ pico PENDING"); }
int board_boot_generation(void) { return transact("BOOT esp"); }
