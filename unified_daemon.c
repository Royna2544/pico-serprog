#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>

#include <unistd.h>

#define MAX_CHILDREN 8

struct child {
  const char *name;
  pid_t pid;
  int output;
  char line[2048];
  size_t used;
  bool alive;
};

static struct child children[MAX_CHILDREN];
static size_t child_count;
static volatile sig_atomic_t stopping;

static void on_signal(int signum) { stopping = signum; }

static int path_join(char *out, size_t size, const char *dir,
                     const char *name) {
  int n = snprintf(out, size, "%s/%s", dir, name);
  if (n < 0 || (size_t)n >= size) {
    errno = ENAMETOOLONG;
    return -1;
  }
  return 0;
}

static void print_line(struct child *child) {
  if (!child->used) return;
  child->line[child->used] = 0;
  // The boardstate daemon owns the physical-event names; don't double-prefix.
  if (!strcmp(child->name, "boardstate-daemon") &&
      (!strncmp(child->line, "pico> ", 6) ||
       !strncmp(child->line, "esp> ", 5) ||
       !strncmp(child->line, "uno> ", 5) ||
       !strncmp(child->line, "uart> ", 6)))
    printf("%s\n", child->line);
  else
    printf("%s> %s\n", child->name, child->line);
  fflush(stdout);
  child->used = 0;
}

static void drain_output(struct child *child) {
  char buffer[1024];
  ssize_t n;
  while ((n = read(child->output, buffer, sizeof(buffer))) > 0) {
    for (ssize_t i = 0; i < n; i++) {
      if (buffer[i] == '\n') {
        print_line(child);
      } else if (buffer[i] != '\r') {
        child->line[child->used++] = buffer[i];
        if (child->used == sizeof(child->line) - 1) print_line(child);
      }
    }
  }
  if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) {
    print_line(child);
    close(child->output);
    child->output = -1;
  }
}

static void pump(int timeout_ms) {
  struct pollfd fds[MAX_CHILDREN];
  struct child *owners[MAX_CHILDREN];
  nfds_t count = 0;
  for (size_t i = 0; i < child_count; i++) {
    if (children[i].output < 0) continue;
    fds[count] = (struct pollfd){.fd = children[i].output, .events = POLLIN};
    owners[count++] = &children[i];
  }
  int rc = poll(fds, count, timeout_ms);
  if (rc > 0)
    for (nfds_t i = 0; i < count; i++)
      if (fds[i].revents) drain_output(owners[i]);
}

static int start(const char *name, const char *path, const char *arg1,
                 const char *arg2) {
  if (child_count == MAX_CHILDREN) { errno = E2BIG; return -1; }
  int fds[2];
  if (pipe2(fds, O_CLOEXEC) < 0) return -1;
  pid_t parent = getpid();
  pid_t pid = fork();
  if (pid == 0) {
    close(fds[0]);
    if (prctl(PR_SET_PDEATHSIG, SIGTERM) < 0 || getppid() != parent ||
        dup2(fds[1], STDOUT_FILENO) < 0 ||
        dup2(fds[1], STDERR_FILENO) < 0)
      _exit(127);
    close(fds[1]);
    char *const args[] = {(char *)path, (char *)arg1, (char *)arg2, NULL};
    execvp(path, args);
    perror(path);
    _exit(127);
  }
  close(fds[1]);
  if (pid < 0) { close(fds[0]); return -1; }
  int flags = fcntl(fds[0], F_GETFL);
  if (flags < 0 || fcntl(fds[0], F_SETFL, flags | O_NONBLOCK) < 0) {
    close(fds[0]);
    kill(pid, SIGTERM);
    (void)waitpid(pid, NULL, 0);
    return -1;
  }
  children[child_count++] = (struct child){
      .name = name, .pid = pid, .output = fds[0], .alive = true};
  printf("started %s pid=%ld\n", name, (long)pid);
  fflush(stdout);
  return 0;
}

static bool reap(void) {
  int status;
  pid_t pid;
  bool exited = false;
  while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
    for (size_t i = 0; i < child_count; i++) {
      if (children[i].pid != pid) continue;
      children[i].alive = false;
      if (WIFEXITED(status))
        fprintf(stderr, "unified: %s exited with status %d\n",
                children[i].name, WEXITSTATUS(status));
      else if (WIFSIGNALED(status))
        fprintf(stderr, "unified: %s killed by signal %d\n",
                children[i].name, WTERMSIG(status));
      exited = true;
      break;
    }
  }
  return exited;
}

static bool alive_during_startup(int milliseconds) {
  for (int elapsed = 0; elapsed < milliseconds && !stopping; elapsed += 20) {
    pump(20);
    if (reap()) return false;
  }
  return !stopping;
}

static bool socket_ready(const char *path) {
  int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) return false;
  struct sockaddr_un addr = {.sun_family = AF_UNIX};
  strcpy(addr.sun_path, path); // Length checked before starting children.
  bool ready = connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0;
  close(fd);
  return ready;
}

static void terminate_children(void) {
  // Own process group: descendants cannot be accidentally shared with a shell.
  signal(SIGTERM, SIG_IGN);
  kill(-getpgrp(), SIGTERM);
  for (int elapsed = 0; elapsed < 3000; elapsed += 20) {
    pump(20);
    reap();
    bool any = false;
    for (size_t i = 0; i < child_count; i++) any |= children[i].alive;
    if (!any) break;
  }
  for (size_t i = 0; i < child_count; i++)
    if (children[i].alive) kill(children[i].pid, SIGKILL);
  while (waitpid(-1, NULL, 0) > 0 || errno == EINTR) {}
  for (size_t i = 0; i < child_count; i++) {
    if (children[i].output >= 0) {
      drain_output(&children[i]);
      if (children[i].output >= 0) close(children[i].output);
    }
  }
}

int main(int argc, char **argv) {
  const char *build_dir = NULL;
  bool verbose = false;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--verbose")) verbose = true;
    else if (!build_dir && argv[i][0] != '-') build_dir = argv[i];
    else {
      fprintf(stderr, "usage: %s [--verbose] <host-build-dir>\n", argv[0]);
      return 2;
    }
  }
  if (!build_dir) {
    fprintf(stderr, "usage: %s [--verbose] <host-build-dir>\n", argv[0]);
    return 2;
  }
  char resolved[PATH_MAX];
  if (!realpath(build_dir, resolved)) { perror(build_dir); return 1; }
  if (getpgrp() != getpid() && setpgid(0, 0) < 0) {
    perror("setpgid");
    return 1;
  }
  struct sigaction handler = {.sa_handler = on_signal};
  sigemptyset(&handler.sa_mask);
  if (sigaction(SIGTERM, &handler, NULL) < 0 ||
      sigaction(SIGINT, &handler, NULL) < 0 ||
      sigaction(SIGHUP, &handler, NULL) < 0) {
    perror("sigaction");
    return 1;
  }
  const char *tmp = getenv("TMPDIR");
  if (!tmp || !*tmp) tmp = "/tmp";
  char run_dir[PATH_MAX], sock[PATH_MAX], dtb[PATH_MAX], executable[PATH_MAX];
  if (path_join(run_dir, sizeof(run_dir), tmp, "unified-board-XXXXXX") < 0 ||
      !mkdtemp(run_dir)) {
    perror("private run directory");
    return 1;
  }
  if (path_join(sock, sizeof(sock), run_dir, "board.sock") < 0 ||
      strlen(sock) >= sizeof(((struct sockaddr_un *)0)->sun_path) ||
      path_join(dtb, sizeof(dtb), resolved, "board.dtb") < 0) {
    fprintf(stderr, "unified: board socket or DTB path too long\n");
    rmdir(run_dir);
    return 1;
  }
  if (setenv("BOARDSTATE_SOCKET", sock, 1) < 0 ||
      setenv("MOCK_PTY_DIR", run_dir, 1) < 0 ||
      setenv("BOARDSTATE_VERBOSE", verbose ? "1" : "0", 1) < 0) {
    perror("setenv");
    rmdir(run_dir);
    return 1;
  }
  printf("BOARDSTATE_SOCKET=%s\nMOCK_PTY_DIR=%s\n", sock, run_dir);
  fflush(stdout);
  int result = 1;
  const char *ports[] = {"uart0", "cdc0", "cdc1", "cdc2"};
  for (size_t i = 0; i < sizeof(ports) / sizeof(ports[0]); i++) {
    char a[PATH_MAX], b[PATH_MAX];
    int n = snprintf(a, sizeof(a), "pty,raw,echo=0,link=%s/mock-%s", run_dir, ports[i]);
    int m = snprintf(b, sizeof(b), "pty,raw,echo=0,link=%s/host-%s", run_dir, ports[i]);
    if (n < 0 || (size_t)n >= sizeof(a) || m < 0 || (size_t)m >= sizeof(b) ||
        start("socat", "socat", a, b) < 0) goto cleanup;
  }
  for (int elapsed = 0; elapsed < 3000; elapsed += 20) {
    bool ready = true;
    for (size_t i = 0; i < sizeof(ports) / sizeof(ports[0]); i++) {
      char path[PATH_MAX];
      if (snprintf(path, sizeof(path), "%s/mock-%s", run_dir, ports[i]) >= (int)sizeof(path) ||
          access(path, F_OK) < 0) ready = false;
      if (snprintf(path, sizeof(path), "%s/host-%s", run_dir, ports[i]) >= (int)sizeof(path) ||
          access(path, F_OK) < 0) ready = false;
    }
    if (ready) break;
    if (!alive_during_startup(20)) goto cleanup;
    if (elapsed == 2980) { fprintf(stderr, "unified: PTY startup timed out\n"); goto cleanup; }
  }
  if (path_join(executable, sizeof(executable), resolved, "boardstate-daemon") < 0 ||
      start("boardstate-daemon", executable, dtb, sock) < 0) goto cleanup;
  bool ready = false;
  for (int elapsed = 0; elapsed < 3000; elapsed += 20) {
    if (socket_ready(sock)) { ready = true; break; }
    if (!alive_during_startup(20)) goto cleanup;
  }
  if (!ready || stopping) {
    fprintf(stderr, "unified: boardstate socket did not become ready\n");
    goto cleanup;
  }
  if (path_join(executable, sizeof(executable), resolved, "vpmic-mock") < 0 ||
      start("vpmic-mock", executable, NULL, NULL) < 0 ||
      !alive_during_startup(100)) goto cleanup;
  if (path_join(executable, sizeof(executable), resolved, "esp8266-mock") < 0 ||
      start("esp8266-mock", executable, NULL, NULL) < 0 ||
      !alive_during_startup(100)) goto cleanup;
  if (path_join(executable, sizeof(executable), resolved, "pico_serprog") < 0 ||
      start("pico_serprog", executable, NULL, NULL) < 0) goto cleanup;
  result = 0;
  while (!stopping) {
    pump(50);
    if (reap()) { result = 1; break; }
  }
cleanup:
  if (result) fprintf(stderr, "unified: startup/child failure\n");
  terminate_children();
  unlink(sock); // Private, freshly created directory only.
  for (size_t i = 0; i < sizeof(ports) / sizeof(ports[0]); i++) {
    char path[PATH_MAX];
    if (snprintf(path, sizeof(path), "%s/mock-%s", run_dir, ports[i]) < (int)sizeof(path)) unlink(path);
    if (snprintf(path, sizeof(path), "%s/host-%s", run_dir, ports[i]) < (int)sizeof(path)) unlink(path);
  }
  if (rmdir(run_dir) < 0) perror("unified: remove run directory");
  return result;
}