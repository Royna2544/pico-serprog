// Test-only path mapping for mock-pico-sdk's fixed /tmp/mock-* endpoints.
// No /tmp files or symlinks are created, and production code is unchanged.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

__attribute__((constructor)) static void line_buffered_output(void) {
  setvbuf(stdout, NULL, _IOLBF, 0);
}

int open(const char *path, int flags, ...) {
  int (*real_open)(const char *, int, ...) = dlsym(RTLD_NEXT, "open");
  const char *names[] = {"uart0", "uart1", "cdc0", "cdc1", "cdc2"};
  for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
    char original[64], variable[64];
    snprintf(original, sizeof(original), "/tmp/mock-%s", names[i]);
    if (strcmp(path, original))
      continue;
    snprintf(variable, sizeof(variable), "FAKENORFLASH_PTY_%s", names[i]);
    const char *mapped = getenv(variable);
    if (mapped)
      path = mapped;
    break;
  }
  if ((flags & O_CREAT) || (flags & O_TMPFILE) == O_TMPFILE) {
    va_list args;
    va_start(args, flags);
    mode_t mode = va_arg(args, mode_t);
    va_end(args);
    return real_open(path, flags, mode);
  }
  return real_open(path, flags);
}
