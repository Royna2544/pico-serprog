/* Exercise the real passthru encoding callback with UART/USB boundaries stubbed. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <hardware/uart.h>
#include "core/driver.h"

#undef PLATFORM_DRIVER
#define PLATFORM_DRIVER(plat)
#include "drivers/passthru.c"

static uart_line_coding_t applied;
static int format_calls, baud_calls;

void printk(int level, const char *fmt, ...) {
  (void)level;
  (void)fmt;
}

void uart_set_line_coding(const struct device *dev,
                          const uart_line_coding_t *coding) {
  (void)dev;
  applied = *coding;
  format_calls++;
}

void uart_set_baud(const struct device *dev, unsigned int baud) {
  (void)dev;
  assert(baud == 115200);
  baud_calls++;
}

static void check_parity(uint8_t cdc_parity, uart_parity_t expected) {
  struct device uart = {.name = "uart1"};
  struct passthru_driver_pdata pdata = {.uart_dev = &uart};
  usb_cdc_line_coding_t coding = {
      .bit_rate = 115200, .stop_bits = 0, .parity = cdc_parity, .data_bits = 8};
  format_calls = baud_calls = 0;
  passthru_encoding_changed_callback(&coding, &pdata);
  assert(format_calls == 1 && baud_calls == 1);
  assert(applied.parity == expected);
  assert(applied.stop_bits == 1 && applied.data_bits == 8);
}

static void check_unsupported_parity(uint8_t cdc_parity) {
  struct device uart = {.name = "uart1"};
  struct passthru_driver_pdata pdata = {.uart_dev = &uart};
  usb_cdc_line_coding_t coding = {
      .bit_rate = 115200, .stop_bits = 0, .parity = cdc_parity, .data_bits = 8};
  format_calls = baud_calls = 0;
  passthru_encoding_changed_callback(&coding, &pdata);
  assert(format_calls == 0 && baud_calls == 0);
}

int main(int argc, char **argv) {
  if (argc > 1) {
    (void)argv;
    check_unsupported_parity(3);
    check_unsupported_parity(4);
    puts("PASS: unsupported CDC parity is rejected");
    return 0;
  }
  check_parity(0, UART_PARITY_NONE);
  check_parity(1, UART_PARITY_ODD);
  check_parity(2, UART_PARITY_EVEN);
  puts("PASS: CDC parity maps to Pico UART parity");
  return 0;
}
