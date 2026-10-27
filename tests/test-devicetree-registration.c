#include <stdarg.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

#include "core/devicetree.c"

LinkedList gDevices;
struct driver *driver_find_by_compatible(const char *compatible) {
  (void)compatible;
  return NULL;
}
bool device_register(struct device *dev) {
  (void)dev;
  return false;
}
void printk(int level, const char *fmt, ...) {
  (void)level;
  (void)fmt;
}
void do_panic(const char *fmt, ...) {
  (void)fmt;
  abort();
}

int main(void) {
  int rc = devicetree_init();
  if (rc != -ENODEV) {
    fprintf(stderr, "device tree did not propagate registration failure: %d\n",
            rc);
    return 1;
  }
  return 0;
}
