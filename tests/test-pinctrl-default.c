#include "core/devicetree.h"
#include "core/printk.h"
#include <libfdt.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

void printk(int level, const char *fmt, ...) {
  (void)level;
  (void)fmt;
}
struct device *device_find_by_offset(const void *fdt, int offset) {
  (void)fdt;
  (void)offset;
  return NULL;
}
LinkedList gDevices;
struct driver *driver_find_by_compatible(const char *compatible) {
  (void)compatible;
  return NULL;
}
bool device_register(struct device *dev) {
  (void)dev;
  return false;
}
void do_panic(const char *fmt, ...) {
  (void)fmt;
  abort();
}

int main(void) {
  char blob[1024];
  if (fdt_create_empty_tree(blob, sizeof(blob))) return 2;
  int uart = fdt_add_subnode(blob, 0, "serial");
  if (uart < 0 || fdt_setprop_string(blob, uart, "pinctrl-names", "default"))
    return 2;
  struct device dev = {.name = "serial", .fdt = blob, .fdt_node_offset = uart};
  int rc = apply_pinctrl_default(&dev);
  if (rc != -EINVAL) {
    fprintf(stderr, "broken declared default pinctrl accepted: %d\n", rc);
    return 1;
  }
  if (fdt_setprop(blob, uart, "pinctrl-0", NULL, 0)) return 2;
  if (apply_pinctrl_default(&dev) != -EINVAL) return 1;
  int no_pinctrl = fdt_add_subnode(blob, 0, "no-pinctrl");
  if (no_pinctrl < 0) return 2;
  dev.fdt_node_offset = no_pinctrl;
  if (apply_pinctrl_default(&dev) != -ENOENT) return 1;
  dev.fdt = NULL;
  if (apply_pinctrl_default(&dev) != -ENOENT) return 1;
  return 0;
}
