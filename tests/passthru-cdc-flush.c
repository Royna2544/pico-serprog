/* Exercise real passthru loop with UART and CDC FIFO boundaries substituted. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "core/driver.h"

#undef PLATFORM_DRIVER
#define PLATFORM_DRIVER(plat)
#include "drivers/passthru.c"

static bool *stop_loop;
static uint8_t pending, delivered;
static int pending_count, delivered_count, flush_count;

void printk(int level, const char *fmt, ...) {
  (void)level;
  (void)fmt;
}

bool uart_readable(const struct device *dev) {
  (void)dev;
  *stop_loop = true;
  return true;
}

int uart_read_nbytes(const struct device *dev, uint8_t *dst, size_t len) {
  (void)dev;
  assert(len == 1);
  *dst = 0xc0;
  return 1;
}

uint32_t usb_cdc_write_bytes(const struct device *dev, const void *buf,
                             uint32_t len) {
  (void)dev;
  assert(len == 1);
  pending = *(const uint8_t *)buf;
  pending_count++;
  return len;
}

void usb_cdc_write_flush(const struct device *dev) {
  (void)dev;
  assert(pending_count == 1);
  delivered = pending;
  delivered_count += pending_count;
  pending_count = 0;
  flush_count++;
}

uint32_t usb_cdc_read_available(const struct device *dev) {
  (void)dev;
  return 0;
}

uint32_t usb_cdc_read_bytes(const struct device *dev, void *buf, uint32_t len) {
  (void)dev;
  (void)buf;
  (void)len;
  assert(false);
  return 0;
}

int uart_write_nbytes(const struct device *dev, const uint8_t *src, size_t len) {
  (void)dev;
  (void)src;
  (void)len;
  assert(false);
  return 0;
}

void uart_set_line_coding(const struct device *dev,
                          const uart_line_coding_t *coding) {
  (void)dev;
  (void)coding;
}

void uart_set_baud(const struct device *dev, unsigned int baud) {
  (void)dev;
  (void)baud;
}

static int register_callbacks(const struct device *dev,
                              usb_cdc_io_callback_t read_available,
                              usb_cdc_io_callback_t write_done,
                              usb_cdc_encoding_changed_callback_t changed,
                              void *param) {
  (void)dev;
  (void)read_available;
  (void)write_done;
  (void)changed;
  (void)param;
  return 0;
}

int main(void) {
  struct usb_cdc_driver cdc_driver = {
      .drv = {.name = "cdc"}, .register_io_callbacks = register_callbacks};
  struct device uart = {.name = "uart1"};
  struct device cdc = {.name = "cdc2", .drv = &cdc_driver.drv};
  struct passthru_driver_pdata pdata = {
      .uart_dev = &uart, .usb_cdc_dev = &cdc};
  struct device passthru = {.name = "passthru", .platform_data = &pdata};
  bool stop = false;
  stop_loop = &stop;
  passthru_loop(&passthru, &stop);
  assert(delivered_count == 1 && delivered == 0xc0 && flush_count == 1);
  puts("PASS: a sub-packet UART reply reaches CDC without waiting for 64 bytes");
  return 0;
}
