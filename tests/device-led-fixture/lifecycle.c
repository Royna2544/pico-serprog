/* Host-only lifecycle regressions; compile with run.sh. Production .c is
 * included so its static controller and deferred-init callbacks are exercised.
 * Only the hardware/DT boundary and allocator are replaced by the fixture. */
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libfdt.h>

#include "core/device.c"
#include "drivers/leddrv.c"

void *__real_malloc(size_t size);
void *__real_calloc(size_t count, size_t size);
void __real_free(void *ptr);

#define CHECK(expr)                                                            \
  do {                                                                         \
    if (!(expr)) {                                                             \
      fprintf(stderr, "%s:%d: FAILED: %s\n", __FILE__, __LINE__, #expr);  \
      exit(1);                                                                 \
    }                                                                          \
  } while (0)

static void *live[64];
static int live_count, calloc_count, fail_calloc_at;
static int pinctrl_request_count, pinctrl_release_count, pinctrl_error;
static const char *pinctrl_error_name;
static int gpio_requests, gpio_releases;
static const char *gpio_error_name;
static int probe_count, remove_count;
static char tree[4096];

static void remember(void *ptr) {
  if (!ptr)
    return;
  CHECK(live_count < (int)(sizeof(live) / sizeof(live[0])));
  live[live_count++] = ptr;
}
void *__wrap_malloc(size_t size) {
  void *p = __real_malloc(size);
  remember(p);
  return p;
}
void *__wrap_calloc(size_t count, size_t size) {
  calloc_count++;
  if (calloc_count == fail_calloc_at)
    return NULL;
  void *p = __real_calloc(count, size);
  remember(p);
  return p;
}
void __wrap_free(void *ptr) {
  if (!ptr)
    return;
  for (int n = 0; n < live_count; n++) {
    if (live[n] == ptr) {
      live[n] = live[--live_count];
      __real_free(ptr);
      return;
    }
  }
  fprintf(stderr, "unexpected free %p\n", ptr);
  exit(2);
}

void printk(int level, const char *fmt, ...) { (void)level; (void)fmt; }
uint64_t time_us_64(void) { return 0; }
void sleep_ms(uint32_t ms) { (void)ms; }
static void no_free(struct device *dev) { (void)dev; }
int apply_pinctrl_default(struct device *dev) {
  pinctrl_request_count++;
  if (pinctrl_error_name && !strcmp(dev->name, pinctrl_error_name))
    return pinctrl_error;
  return -ENOENT;
}
int remove_pinctrl_default(struct device *dev) {
  (void)dev;
  pinctrl_release_count++;
  return 0;
}
struct gpio_desc *gpio_request(struct device *dev, int node, const char *name) {
  (void)name;
  const char *child_name = fdt_get_name(dev->fdt, node, NULL);
  gpio_requests++;
  if (gpio_error_name && !strcmp(child_name, gpio_error_name))
    return NULL;
  struct gpio_desc *desc = calloc(1, sizeof(*desc));
  CHECK(desc);
  return desc;
}
void gpio_desc_release(struct device *dev, struct gpio_desc *desc) {
  (void)dev;
  CHECK(desc);
  gpio_releases++;
  free(desc);
}
int gpio_set_value(struct gpio_desc *desc, bool state) {
  (void)desc;
  (void)state;
  return 0;
}

static int noop_probe(struct device *dev) {
  probe_count++;
  (void)dev;
  return 0;
}
static int fail_probe(struct device *dev) {
  probe_count++;
  (void)dev;
  return -EIO;
}
static void needs_probed_data(struct device *dev) {
  CHECK(dev->platform_data != NULL); /* spidrv_master_remove dereferences it */
  remove_count++;
}
static struct driver dummy_drv = {.name = "dummy", .probe = noop_probe,
                                  .remove = needs_probed_data};
static struct driver failing_drv = {.name = "failing", .probe = fail_probe};

static void reset(void) {
  CHECK(!live_count);
  device_init();
  driver_init();
  calloc_count = fail_calloc_at = 0;
  pinctrl_request_count = pinctrl_release_count = pinctrl_error = 0;
  pinctrl_error_name = gpio_error_name = NULL;
  gpio_requests = gpio_releases = 0;
  probe_count = remove_count = 0;
}
static void check_clean(void) {
  CHECK(!gDevices.head);
  CHECK(live_count == 0);
}
static struct device basic_device(const char *name, struct driver *drv) {
  struct device dev = {.name = name, .drv = drv, .free = no_free};
  return dev;
}
static struct device controller(int led_count) {
  CHECK(fdt_create_empty_tree(tree, sizeof(tree)) == 0);
  int root = fdt_add_subnode(tree, 0, "led-controller");
  CHECK(root > 0);
  if (led_count > 1)
    CHECK(fdt_add_subnode(tree, root, "led-b") > 0);
  if (led_count > 0)
    CHECK(fdt_add_subnode(tree, root, "led-a") > 0);
  struct device dev = basic_device("led-controller", &pico_led_controller_driver.drv);
  dev.fdt = tree;
  dev.fdt_node_offset = root;
  return dev;
}

static void test_pinctrl_failure_no_node_leak(void) {
  reset();
  struct device dev = basic_device("spi-master", &dummy_drv);
  pinctrl_error_name = "spi-master";
  pinctrl_error = -EIO;
  CHECK(!device_register(&dev));
  CHECK(pinctrl_request_count == 1);
  CHECK(probe_count == 0);
  CHECK(remove_count == 0);
  CHECK(dev.drv == &dummy_drv); /* failure must not unbind an unprobed device */
  check_clean();
}
static void test_failed_probe_releases_pinctrl(void) {
  reset();
  struct device dev = basic_device("failed-probe", &failing_drv);
  pinctrl_error_name = dev.name;
  pinctrl_error = 0; /* successfully acquired pinctrl state */
  CHECK(!device_register(&dev));
  CHECK(probe_count == 1);
  CHECK(pinctrl_release_count == 1);
  check_clean();
}
static void test_deferred_unprobed_remove_is_not_called(void) {
  reset();
  struct device parent = basic_device("unbound-parent", NULL);
  struct device dev = basic_device("spi-master", &dummy_drv);
  dev.parent = &parent;
  CHECK(device_register(&dev));
  CHECK(dev.deferred);
  CHECK(probe_count == 0);
  CHECK(device_unregister(&dev));
  CHECK(remove_count == 0);
  check_clean();
}
static void test_retry_then_remove_once(void) {
  reset();
  struct device parent = basic_device("unbound-parent", NULL);
  struct device dev = basic_device("spi-master", &dummy_drv);
  int probed_data = 1;
  dev.parent = &parent;
  CHECK(device_register(&dev));
  parent.drv = &dummy_drv;
  dev.platform_data = &probed_data;
  CHECK(retry_deferred() == 0);
  CHECK(!dev.deferred);
  CHECK(probe_count == 1);
  CHECK(device_unregister(&dev));
  CHECK(remove_count == 1);
  check_clean();
}
static void test_led_allocation_failure(void) {
  for (int failure = 1; failure <= 4; failure++) {
    reset();
    struct device dev = controller(1);
    fail_calloc_at = failure;
    CHECK(!device_register(&dev));
    CHECK(gpio_requests == 0);
    CHECK(gpio_releases == 0);
    check_clean();
  }
}
static void test_led_register_failure_releases_current_descriptor(void) {
  reset();
  struct device dev = controller(2);
  pinctrl_error_name = "led-b";
  pinctrl_error = -EIO;
  CHECK(!device_register(&dev));
  CHECK(gpio_requests == 2);
  CHECK(gpio_releases == 2);
  CHECK(dev.platform_data == NULL);
  check_clean();
}
static void test_led_gpio_failure_unwinds_registered_sibling(void) {
  reset();
  struct device dev = controller(2);
  gpio_error_name = "led-b";
  CHECK(!device_register(&dev));
  CHECK(gpio_requests == 2);
  CHECK(gpio_releases == 1);
  check_clean();
}
static void test_led_success_removes_each_once(void) {
  reset();
  struct device dev = controller(2);
  CHECK(device_register(&dev));
  CHECK(gpio_requests == 2);
  CHECK(device_unregister(&dev));
  CHECK(gpio_releases == 2);
  check_clean();
}

int main(int argc, char **argv) {
  if (argc != 2) {
    fprintf(stderr, "usage: %s CASE\n", argv[0]);
    return 2;
  }
#define RUN(name) if (!strcmp(argv[1], #name)) { test_##name(); puts("PASS " #name); return 0; }
  RUN(pinctrl_failure_no_node_leak)
  RUN(failed_probe_releases_pinctrl)
  RUN(deferred_unprobed_remove_is_not_called)
  RUN(retry_then_remove_once)
  RUN(led_allocation_failure)
  RUN(led_register_failure_releases_current_descriptor)
  RUN(led_gpio_failure_unwinds_registered_sibling)
  RUN(led_success_removes_each_once)
#undef RUN
  fprintf(stderr, "unknown case: %s\n", argv[1]);
  return 2;
}
