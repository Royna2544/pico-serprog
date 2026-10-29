#include <stdbool.h>
#include <string.h>
#include "drivers/serprog.c"
#include <stdio.h>

static bool selected, selected_while_reading_payload, short_payload;
static bool fail_transfer;
static int phase, replies, naks, asserts, deasserts;

void printk(int level, const char *fmt, ...) {
  (void)level;
  (void)fmt;
}
bool driver_register(struct driver *drv) {
  (void)drv;
  return true;
}
struct device *find_device_by_prop(struct device *dev, const char *name,
                                   int offset, int index) {
  (void)dev;
  (void)name;
  (void)offset;
  (void)index;
  return NULL;
}
uint32_t usb_cdc_read_byte(const struct device *dev, uint8_t *byte) {
  (void)dev;
  *byte = S_CMD_O_SPIOP;
  return 1;
}
uint32_t usb_cdc_read_bytes(const struct device *dev, void *buf, uint32_t len) {
  (void)dev;
  if (phase++ == 0) {
    if (len != 6) return 0;
    const uint8_t header[] = {3, 0, 0, 0, 0, 0};
    memcpy(buf, header, sizeof(header));
  } else {
    if (selected) selected_while_reading_payload = true;
    if (len != 3) return 0;
    const uint8_t payload[] = {0x9f, 0, 0};
    memcpy(buf, payload, sizeof(payload));
    if (short_payload) return len - 1;
  }
  return len;
}
uint32_t usb_cdc_write_byte(const struct device *dev, uint8_t byte) {
  (void)dev;
  if (byte == S_ACK) replies++;
  if (byte == S_NAK) naks++;
  return 1;
}
uint32_t usb_cdc_write_bytes(const struct device *dev, const void *buf,
                             uint32_t len) {
  (void)dev;
  (void)buf;
  return len;
}
void usb_cdc_write_flush(const struct device *dev) { (void)dev; }
int spi_chip_assert_cs(const struct device *dev) {
  (void)dev;
  selected = true;
  asserts++;
  return 0;
}
int spi_chip_deassert_cs(const struct device *dev) {
  (void)dev;
  selected = false;
  deasserts++;
  return 0;
}
int spi_chip_write_nbytes(const struct device *dev, const void *buf,
                          uint32_t len) {
  (void)dev;
  (void)buf;
  return fail_transfer ? len - 1 : len;
}
int spi_chip_read_nbytes(const struct device *dev, void *buf, uint32_t len) {
  (void)dev;
  (void)buf;
  return len;
}
int spi_chip_set_baud(const struct device *dev, uint32_t baud) {
  (void)dev;
  (void)baud;
  return 0;
}
uint32_t spi_chip_get_baud(const struct device *dev) {
  (void)dev;
  return 1000000;
}

int main(void) {
  struct device cdc = {.name = "cdc"};
  struct device chip = {.name = "chip"};
  struct serprog_platform_data pdata = {
      .usb_cdc = &cdc, .spi_chip = &chip};
  struct device dev = {.name = "serprog", .platform_data = &pdata};
  serprogdrv_handle_command(&dev);
  if (selected_while_reading_payload || selected || replies != 1 ||
      asserts != 1 || deasserts != 1) {
    fprintf(stderr, "CS asserted during CDC payload read: %d\n",
            selected_while_reading_payload);
    return 1;
  }
  phase = replies = asserts = deasserts = 0;
  short_payload = true;
  serprogdrv_handle_command(&dev);
  if (selected || asserts || deasserts || replies || naks) {
    fprintf(stderr, "incomplete host payload changed CS or sent a reply\n");
    return 1;
  }
  phase = 0;
  short_payload = false;
  fail_transfer = true;
  serprogdrv_handle_command(&dev);
  if (selected || asserts != 1 || deasserts != 1 || replies || naks != 1) {
    fprintf(stderr, "short SPI write did not deassert CS and NAK\n");
    return 1;
  }
  puts("PASS: serprog payload before CS, short payload quiet, failed SPI NAK");
  return 0;
}
