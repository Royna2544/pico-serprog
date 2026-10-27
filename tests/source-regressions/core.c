/* The IRQ controller lookup is a fixture; registration and FDT matching are real. */
#include <assert.h>
#include <stdlib.h>
#define has_property fixture_has_property
#define find_device_by_prop fixture_find_device_by_prop
#include "core/device.c"
#undef has_property
#undef find_device_by_prop
#include "core/irq.c"
#include "core/devicetree.c"

const uint8_t board_dtb_start[1024] = {0};
const uint8_t board_dtb_end[1] = {0};
void printk(int level, const char *format, ...) {}
void do_panic(const char *format, ...) { abort(); }
static bool with_irq;
static struct device controller = {.name = "controller"};
bool fixture_has_property(int node, const char *name) { return with_irq; }
struct device *fixture_find_device_by_prop(struct device *dev, const char *name,
                                           int node, int index) {
  return &controller;
}
static int probes, unmaps, map_error, xlate_error, marker;
static const void *expected_data;
static int probe(struct device *dev) {
  assert(dev->driver_data == expected_data);
  probes++;
  return 0;
}
static int xlate(struct irq_domain *d, int node, const uint32_t *spec, int count,
                  int *hw, irq_event_t *event) {
  *hw = 22;
  *event = IRQ_EVENT_RISING_EDGE;
  return xlate_error;
}
static int map(struct irq_domain *domain, struct irq_data *data) { return map_error; }
static int unmap(struct irq_domain *domain, struct irq_data *data) {
  unmaps++;
  return 0;
}

int main(int argc, char **argv) {
  assert(argc == 2);
  char blob[2048];
  assert(fdt_create_empty_tree(blob, sizeof(blob)) == 0);
  int node = fdt_add_subnode(blob, 0, "consumer");
  struct driver_fdt_match_id ids[] = {
    {.compatible = "first"}, {.compatible = "second", .data = &marker}, {0}};
  struct driver drv = {.name = "test", .probe = probe, .match_table = ids};
  struct device *dev = calloc(1, sizeof(*dev));
  assert(dev);
  *dev = (struct device){.name = "test-device", .drv = &drv, .fdt = blob,
                         .fdt_node_offset = node};
  if (!strcmp(argv[1], "matching") || !strcmp(argv[1], "fallback")) {
    free(dev);
    driver_register(&drv);
    const char compatible[] = "unmatched\0second";
    if (!strcmp(argv[1], "fallback"))
      assert(fdt_setprop(blob, node, "compatible", compatible, sizeof(compatible)) == 0);
    else
      assert(fdt_setprop_string(blob, node, "compatible", "second") == 0);
    expected_data = &marker;
    add_recursive_devices(blob, node, NULL);
    dev = device_find_by_offset(blob, node);
    assert(dev && dev->drv == &drv && dev->driver_data == &marker && probes == 1);
    assert(device_unregister(dev));
    list_del(&drv.list);
    return 0;
  }
  if (!strcmp(argv[1], "deferred")) {
    dev->parent = &controller;
    assert(device_register(dev) && dev->deferred);
    assert(retry_deferred() == 0 && probes == 0);
    assert(device_unregister(dev));
    return 0;
  }
  with_irq = true;
  fdt32_t spec[] = {cpu_to_fdt32(1), cpu_to_fdt32(22), cpu_to_fdt32(1)};
  assert(fdt_setprop(blob, node, "interrupts-extended", spec, sizeof(spec)) == 0);
  if (!strcmp(argv[1], "missing-domain")) {
    assert(device_register(dev) && dev->deferred && probes == 0);
    assert(device_unregister(dev));
    return 0;
  }
  struct irq_domain_ops ops = {.xlate = xlate, .map = map, .unmap = unmap};
  if (!strcmp(argv[1], "no-xlate")) ops.xlate = NULL;
  if (!strcmp(argv[1], "no-map")) ops.map = NULL;
  if (!strcmp(argv[1], "xlate-failure")) xlate_error = -EIO;
  if (!strcmp(argv[1], "map-failure")) map_error = -EIO;
  struct irq_domain *domain = irq_domain_create(&controller, 0, 30,
      !strcmp(argv[1], "no-ops") ? NULL : &ops);
  assert(domain);
  assert(!device_register(dev) && probes == 0 && !dev->drv);
  assert(list_empty(&domain->map_list));
  if (map_error) {
    assert(unmaps == 1);
    map_error = 0;
    dev->drv = &drv;
    assert(device_register(dev) && probes == 1); // Mapping can be retried.
    assert(device_unregister(dev));
  } else {
    free(dev);
  }
  irq_domain_destroy(domain);
  return 0;
}
