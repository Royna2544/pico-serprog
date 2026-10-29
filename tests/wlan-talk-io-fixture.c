/* Exercise the real WLAN transport with short/error UART boundaries. */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>

#include "drivers/wlandrv.c"

static int fake_write, fake_read;
static int writes, reads;

void printk(int level, const char *fmt, ...) {
  (void)level;
  (void)fmt;
}
/* Other driver entry points remain linked through the registration record. */
bool driver_register(struct driver *drv) { (void)drv; return true; }
int gpio_request_irq(struct device *dev, struct device *requester,
                     int offset, const char *name) {
  (void)dev; (void)requester; (void)offset; (void)name; return -1;
}
struct gpio_desc *gpio_request(struct device *dev, int offset, const char *name) {
  (void)dev; (void)offset; (void)name; return NULL;
}
int get_prop_bool_default(struct device *dev, const char *name,
                          bool *value, bool fallback) {
  (void)dev; (void)name; *value = fallback; return 0;
}
struct device *find_device_by_prop(struct device *dev, const char *name,
                                   int offset, int index) {
  (void)dev; (void)name; (void)offset; (void)index; return NULL;
}
int latched_switch_get_state(struct device *dev, bool *state) {
  (void)dev; (void)state; return -1;
}
int latched_switch_set_on_change_callback(struct device *dev,
    latched_switch_on_change_callback_t callback, void *data) {
  (void)dev; (void)callback; (void)data; return -1;
}
void gpio_desc_release(struct device *dev, struct gpio_desc *desc) {
  (void)dev; (void)desc;
}
int gpio_set_value(struct gpio_desc *desc, bool value) {
  (void)desc; (void)value; return 0;
}
void gpio_reset(struct gpio_desc *desc) { (void)desc; }
int devm_request_irq(struct device *dev, int irq,
    irqreturn_t (*handler)(int, irq_event_t, void *), void *data) {
  (void)dev; (void)irq; (void)handler; (void)data; return -1;
}
void devm_free_irq(struct device *dev, int irq) { (void)dev; (void)irq; }
uint64_t time_us_64(void) { return 0; }
bool uart_readable(const struct device *dev) { (void)dev; return false; }
int uart_read_nbytes_nonblocking(const struct device *dev, uint8_t *dst, size_t n) {
  (void)dev; (void)dst; (void)n; return -1;
}
int uart_write_nbytes(const struct device *dev, const uint8_t *src, size_t len) {
  (void)dev;
  (void)src;
  (void)len;
  writes++;
  return fake_write;
}
int uart_read_nbytes(const struct device *dev, uint8_t *dst, size_t len) {
  (void)dev;
  (void)len;
  reads++;
  if (fake_read > 0)
    dst[0] = 0x41;
  return fake_read;
}

static int check(const char *what, int actual, int expected) {
  if (actual == expected) return 0;
  fprintf(stderr, "%s: got %d expected %d\n", what, actual, expected);
  return 1;
}

int main(void) {
  struct esp8266_platform_data state = {.cur_boot_mode = WLAN_BOOT_NORMAL};
  struct device uart = {.name = "uart1"};
  struct device wifi = {.name = "wifi", .platform_data = &state,
                        .parent = &uart};
  char byte = 0;
  int failures = 0;
  fake_write = 1;
  failures += check("full write", esp8266_talk(&wifi, "AB", 1, NULL, 0), 0);
  fake_write = 0;
  failures += check("short write", esp8266_talk(&wifi, "AB", 2, NULL, 0), -EIO);
  fake_write = -ENODEV;
  failures += check("write error", esp8266_talk(&wifi, "AB", 2, NULL, 0), -ENODEV);
  fake_read = 1;
  failures += check("full read", esp8266_talk(&wifi, NULL, 0, &byte, 1), 0);
  failures += check("read byte", byte, 0x41);
  fake_read = 0;
  failures += check("short read", esp8266_talk(&wifi, NULL, 0, &byte, 1), -EIO);
  fake_read = -ENODEV;
  failures += check("read error", esp8266_talk(&wifi, NULL, 0, &byte, 1), -ENODEV);
  failures += check("write calls", writes, 3);
  failures += check("read calls", reads, 3);
  if (failures) return 1;
  puts("PASS: WLAN transport rejects short UART I/O and propagates errors");
  return 0;
}
