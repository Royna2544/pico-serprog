#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#define main firmware_main
#include "main.c"
#undef main

static int fail_early(void) { return -7; }
early_initcall(fail_early);

struct device *device_find_by_alias(const char *alias) {
  (void)alias;
  return NULL;
}
void serprogdrv_process_commands(const struct device *dev) { (void)dev; }
void printk(int level, const char *fmt, ...) {
  (void)level;
  (void)fmt;
}

int main(void) { return firmware_main() == EXIT_FAILURE ? 0 : 1; }
