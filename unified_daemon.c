#include <signal.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>

// Take arg: build directory. launches:
// pico_serprog,esp8266-mock,boardstate-daemon

#define BOARDSTATE_DAEMON_SOCKET "/tmp/boardstate-daemon.sock"

void signal_handler(int signum) {
  // Handle termination signals to clean up child processes
  if (signum == SIGINT || signum == SIGTERM) {
    // Send termination signal to all child processes in the same process group
    kill(0, SIGTERM);
  }
}

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "Usage: %s <build_dir>\n", argv[0]);
    return 1;
  }

  if (unlink(BOARDSTATE_DAEMON_SOCKET) == -1) {
    perror("Failed to unlink existing socket");
    // Not a fatal error; continue
  }
  const char *build_dir = argv[1];
  char pathbuf[1024];
  char arg1buf[1024];
  if (snprintf(pathbuf, sizeof(pathbuf), "%s/pico_serprog", build_dir) >=
      sizeof(pathbuf)) {
    fprintf(stderr, "Path too long\n");
    return 1;
  }
  if (snprintf(arg1buf, sizeof(arg1buf), "%s/board.dtb", build_dir) >=
      sizeof(arg1buf)) {
    fprintf(stderr, "Path too long\n");
    return 1;
  }
  setpgrp(); // Make sure child processes are in their own process group

  puts("Starting unified daemon...");
  printf("With arguments: build_dir=%s, board.dtb=%s\n", build_dir, arg1buf);
  // First: boardstate-daemon
  pid_t boardstate_daemon_pid = fork();
  if (boardstate_daemon_pid == 0) {
    // Child process: execute boardstate-daemon
    execlp(pathbuf, "boardstate-daemon", arg1buf, BOARDSTATE_DAEMON_SOCKET,
           NULL);
    perror("execlp failed for boardstate-daemon");
    return 1;
  } else if (boardstate_daemon_pid < 0) {
    perror("fork failed for boardstate-daemon");
    return 1;
  }
  printf("Started boardstate-daemon with PID %d\n", boardstate_daemon_pid);

  puts("Launching esp8266-mock...");
  // Second: esp8266-mock
  snprintf(pathbuf, sizeof(pathbuf), "%s/esp8266-mock", build_dir);
  pid_t esp8266_mock_pid = fork();
  if (esp8266_mock_pid == 0) {
    // Child process: execute esp8266-mock
    execlp(pathbuf, "esp8266-mock", NULL);
    perror("execlp failed for esp8266-mock");
    return 1;
  } else if (esp8266_mock_pid < 0) {
    perror("fork failed for esp8266-mock");
    return 1;
  }
  printf("Started esp8266-mock with PID %d\n", esp8266_mock_pid);

  puts("Launching pico_serprog...");
  snprintf(pathbuf, sizeof(pathbuf), "%s/pico_serprog", build_dir);
  pid_t pico_serprog_pid = fork();
  if (pico_serprog_pid == 0) {
    // Child process: execute pico_serprog
    execlp(pathbuf, "pico_serprog", NULL);
    perror("execlp failed for pico_serprog");
    return 1;
  } else if (pico_serprog_pid < 0) {
    perror("fork failed for pico_serprog");
    return 1;
  }
  printf("Started pico_serprog with PID %d\n", pico_serprog_pid);

  signal(SIGINT, signal_handler);
  signal(SIGTERM, signal_handler);

  // Wait for child processes to finish
  int status;
  if (waitpid(boardstate_daemon_pid, &status, 0) == -1) {
    perror("waitpid failed for boardstate-daemon");
    return 1;
  }
  printf("boardstate-daemon exited with status %d\n", WEXITSTATUS(status));
  if (waitpid(esp8266_mock_pid, &status, 0) == -1) {
    perror("waitpid failed for esp8266-mock");
    return 1;
  }
  printf("esp8266-mock exited with status %d\n", WEXITSTATUS(status));
  if (waitpid(pico_serprog_pid, &status, 0) == -1) {
    perror("waitpid failed for pico_serprog");
    return 1;
  }
  printf("pico_serprog exited with status %d\n", WEXITSTATUS(status));
  return 0;
}