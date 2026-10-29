/* Compile with the mock Pico SDK; call the production UART driver entry points. */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "drivers/uartdrv.c"

static int failures;
#define CHECK_EQ(label, actual, expected)                                       \
  do {                                                                           \
    int got = (actual), want = (expected);                                       \
    if (got != want) {                                                           \
      fprintf(stderr, "%s: got %d, expected %d\n", label, got, want);           \
      failures++;                                                                \
    }                                                                            \
  } while (0)

/* Only the SDK/device-tree boundaries are substituted, not the UART driver. */
void printk(int level, const char *fmt, ...) { (void)level; (void)fmt; }
bool driver_register(struct driver *drv) { (void)drv; return true; }
unsigned int get_prop_uint32_default(struct device *dev, const char *name,
                                     unsigned int *out, unsigned int fallback) {
  (void)dev; (void)name; *out = fallback; return 0;
}
int get_reg(struct device *dev, const char *name, struct reg *out) {
  (void)dev; (void)name; out->base = UART1_BASE; out->size = 0x4000; return 0;
}
int apply_pinctrl_default(const struct device *dev) { (void)dev; return 0; }
int remove_pinctrl_default(const struct device *dev) { (void)dev; return 0; }
unsigned int uart_init(void *hw, unsigned int baud) { (void)hw; return baud; }
unsigned int uart_set_baudrate(void *hw, unsigned int baud) {
  (void)hw; return baud;
}
void uart_deinit(void *hw) { (void)hw; }
bool uart_is_enabled(void *hw) { (void)hw; return true; }

static int read_result, write_result, read_calls, write_calls;
static unsigned read_ready, write_ready, getc_index, putc_index;
static int getc_results[4], putc_results[4];
static uint8_t written[4];
static size_t last_read_len, last_write_len;

int uart_read_blocking(void *hw, uint8_t *dst, size_t len) {
  CHECK_EQ("read UART", hw == uart1, true);
  read_calls++;
  last_read_len = len;
  if (read_result > 0) {
    memset(dst, 0x5a, (size_t)read_result < len ? (size_t)read_result : len);
  }
  return read_result;
}
int uart_write_blocking(void *hw, const uint8_t *src, size_t len) {
  CHECK_EQ("write UART", hw == uart1, true);
  write_calls++;
  last_write_len = len;
  if (len <= sizeof(written)) memcpy(written, src, len);
  return write_result;
}
bool uart_is_readable(void *hw) {
  CHECK_EQ("readable UART", hw == uart1, true);
  if (read_ready) { read_ready--; return true; }
  return false;
}
bool uart_is_writable(void *hw) {
  CHECK_EQ("writable UART", hw == uart1, true);
  if (write_ready) { write_ready--; return true; }
  return false;
}
int uart_getc(void *hw) {
  CHECK_EQ("getc UART", hw == uart1, true);
  if (getc_index >= sizeof(getc_results) / sizeof(getc_results[0])) return -1;
  return getc_results[getc_index++];
}
int uart_putc(void *hw, char c) {
  CHECK_EQ("putc UART", hw == uart1, true);
  if (putc_index < sizeof(written)) written[putc_index] = (uint8_t)c;
  if (putc_index >= sizeof(putc_results) / sizeof(putc_results[0])) return -1;
  return putc_results[putc_index++];
}

int main(void) {
  struct uart_platform_data pdata = {.hw = uart1, .acquired = true};
  struct device dev = {.name = "uart1", .platform_data = &pdata,
                       .drv = &pico_uart_driver.drv};
  uint8_t dst[3] = {0xcc, 0xcc, 0xcc};
  const uint8_t src[3] = {0x31, 0xff, 0x33};

  read_result = 3;
  CHECK_EQ("blocking read success", uart_read_nbytes(&dev, dst, 3), 3);
  CHECK_EQ("blocking read byte", dst[2], 0x5a);
  CHECK_EQ("blocking read requested len", last_read_len, 3);
  read_result = 2;
  dst[2] = 0xcc;
  CHECK_EQ("blocking read short", uart_read_nbytes(&dev, dst, 3), 2);
  CHECK_EQ("blocking read short tail", dst[2], 0xcc);
  read_result = -1;
  CHECK_EQ("blocking read error", uart_read_nbytes(&dev, dst, 3), -1);
  CHECK_EQ("blocking read calls", read_calls, 3);

  write_result = 3;
  CHECK_EQ("blocking write success", uart_write_nbytes(&dev, src, 3), 3);
  CHECK_EQ("blocking write requested len", last_write_len, 3);
  CHECK_EQ("blocking write payload", memcmp(src, written, 3), 0);
  write_result = 1;
  CHECK_EQ("blocking write short", uart_write_nbytes(&dev, src, 3), 1);
  write_result = -1;
  CHECK_EQ("blocking write error", uart_write_nbytes(&dev, src, 3), -1);
  CHECK_EQ("blocking write calls", write_calls, 3);

  read_ready = 2;
  getc_results[0] = 0xff;
  getc_results[1] = 0x12;
  dst[0] = dst[1] = dst[2] = 0xcc;
  CHECK_EQ("nonblocking read valid bytes", uart_read_nbytes_nonblocking(&dev, dst, 3), 2);
  CHECK_EQ("nonblocking read 0xff", dst[0], 0xff);
  CHECK_EQ("nonblocking read second byte", dst[1], 0x12);
  CHECK_EQ("nonblocking read tail", dst[2], 0xcc);
  read_ready = 1;
  getc_index = 0;
  getc_results[0] = -1;
  dst[0] = 0xcc;
  CHECK_EQ("nonblocking read error", uart_read_nbytes_nonblocking(&dev, dst, 1), -1);
  CHECK_EQ("nonblocking read error leaves dst", dst[0], 0xcc);

  write_ready = 1;
  write_result = 1;
  write_calls = 0;
  putc_index = 0;
  putc_results[0] = -1; /* mock uart_putc sign-extends 0xff to -1 */
  CHECK_EQ("nonblocking write 0xff", uart_write_nbytes_nonblocking(&dev, &src[1], 1), 1);
  CHECK_EQ("nonblocking write byte", written[0], 0xff);
  write_ready = 1;
  write_result = -1;
  putc_index = 0;
  putc_results[0] = -1;
  CHECK_EQ("nonblocking write error", uart_write_nbytes_nonblocking(&dev, src, 1), -1);
  write_ready = 1;
  write_result = 0;
  putc_index = 0;
  putc_results[0] = -1;
  CHECK_EQ("nonblocking write no progress", uart_write_nbytes_nonblocking(&dev, src, 1), 0);

  pdata.acquired = false;
  CHECK_EQ("released blocking read", uart_read_nbytes(&dev, dst, 1), -ENODEV);
  CHECK_EQ("released blocking write", uart_write_nbytes(&dev, src, 1), -ENODEV);
  CHECK_EQ("released nonblocking read", uart_read_nbytes_nonblocking(&dev, dst, 1), -ENODEV);
  CHECK_EQ("released nonblocking write", uart_write_nbytes_nonblocking(&dev, src, 1), -ENODEV);
  CHECK_EQ("released read no SDK call", read_calls, 3);
  CHECK_EQ("released write no SDK call", write_calls, 3);
  if (failures) { fprintf(stderr, "UART driver fixture: %d failures\n", failures); return 1; }
  puts("UART driver fixture: read/write short/error, nonblocking and release PASS");
  return 0;
}
