/* Exercise the production serprog command handler, stubbing only CDC/SPI I/O. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "drivers/serprog.c"

enum fault { NO_FAULT, ASSERT_FAIL, WRITE_FAIL, READ_FAIL, DEASSERT_FAIL };
static enum fault fault;
static const char *fault_name[] = {
    "success", "assert CS failure", "short SPI write", "short SPI read",
    "deassert CS failure"};
static struct device cdc = {.name = "cdc"};
static struct device chip = {.name = "chip"};
static int read_phase, selected, asserts, deasserts, writes, reads, callbacks;
static uint8_t response, wire_reply[3], callback_reply[3], callback_send;
static uint32_t wire_reply_len;

static void require(bool condition, const char *what) {
  if (!condition) {
    fprintf(stderr, "FAIL %s: %s (callbacks=%d, response=0x%02x, "
                    "reply=%02x %02x %02x)\n",
            fault_name[fault], what, callbacks, response, callback_reply[0],
            callback_reply[1], callback_reply[2]);
    exit(1);
  }
}

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
  require(dev == &cdc, "unexpected CDC device");
  *byte = S_CMD_O_SPIOP;
  return 1;
}
uint32_t usb_cdc_read_bytes(const struct device *dev, void *buf, uint32_t len) {
  require(dev == &cdc, "unexpected CDC device");
  if (read_phase++ == 0) {
    const uint8_t header[] = {1, 0, 0, 3, 0, 0};
    require(len == sizeof(header), "unexpected SPI header length");
    memcpy(buf, header, len);
  } else {
    require(len == 1, "unexpected SPI send length");
    *(uint8_t *)buf = 0x9f;
  }
  return len;
}
uint32_t usb_cdc_write_byte(const struct device *dev, uint8_t byte) {
  require(dev == &cdc, "unexpected CDC device");
  require(!response, "duplicate ACK/NAK");
  response = byte;
  return 1;
}
uint32_t usb_cdc_write_bytes(const struct device *dev, const void *buf,
                             uint32_t len) {
  require(dev == &cdc, "unexpected CDC device");
  require(len == 3 && !wire_reply_len, "unexpected CDC reply length");
  memcpy(wire_reply, buf, len);
  wire_reply_len = len;
  return len;
}
void usb_cdc_write_flush(const struct device *dev) {
  require(dev == &cdc, "unexpected CDC device");
}
int spi_chip_assert_cs(const struct device *dev) {
  require(dev == &chip, "unexpected SPI chip");
  asserts++;
  if (fault == ASSERT_FAIL)
    return -1;
  selected = 1;
  return 0;
}
int spi_chip_deassert_cs(const struct device *dev) {
  require(dev == &chip, "unexpected SPI chip");
  deasserts++;
  selected = 0;
  return fault == DEASSERT_FAIL ? -1 : 0;
}
int spi_chip_write_nbytes(const struct device *dev, const void *buf,
                          uint32_t len) {
  require(dev == &chip && selected && len == 1 &&
              *(const uint8_t *)buf == 0x9f,
          "unexpected SPI write");
  writes++;
  return fault == WRITE_FAIL ? 0 : (int)len;
}
int spi_chip_read_nbytes(const struct device *dev, void *buf, uint32_t len) {
  require(dev == &chip && selected && len == 3, "unexpected SPI read");
  reads++;
  const uint8_t data[] = {0xef, 0x40, 0x18};
  if (fault == READ_FAIL) {
    memcpy(buf, data, 2); /* third byte retains the previous reply */
    return 2;
  }
  memcpy(buf, data, sizeof(data));
  return (int)len;
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

static void collect_transfer(const struct device *dev, const uint8_t *send,
                             uint32_t slen, uint8_t *reply, uint32_t rlen) {
  require(dev == &chip && !selected && slen == 1 && rlen == 3,
          "callback arguments or CS state invalid");
  callbacks++;
  callback_send = send[0];
  memcpy(callback_reply, reply, rlen);
}

int main(void) {
  struct serprog_platform_data pdata = {.usb_cdc = &cdc, .spi_chip = &chip};
  struct device serprog = {.name = "serprog", .platform_data = &pdata};
  serprogdrv_set_spi_transfer_callback(&serprog, collect_transfer);

  for (enum fault scenario = NO_FAULT; scenario <= DEASSERT_FAIL; scenario++) {
    fault = scenario;
    read_phase = selected = asserts = deasserts = writes = reads = callbacks = 0;
    response = callback_send = 0;
    wire_reply_len = 0;
    memset(wire_reply, 0, sizeof(wire_reply));
    memset(callback_reply, 0, sizeof(callback_reply));
    /* Keep the production receive buffer intact: a failed write leaves the
     * previous successful transfer's reply in it. */
    serprogdrv_handle_command(&serprog);
    require(read_phase == 2 && asserts == 1 && deasserts == 1 && !selected,
            "SPI command did not complete with CS released");
    if (scenario == NO_FAULT) {
      const uint8_t expected[] = {0xef, 0x40, 0x18};
      require(writes == 1 && reads == 1 && callbacks == 1 &&
                  callback_send == 0x9f && response == S_ACK &&
                  wire_reply_len == 3 &&
                  memcmp(callback_reply, expected, sizeof(expected)) == 0 &&
                  memcmp(wire_reply, expected, sizeof(expected)) == 0,
              "successful SPI operation not delivered correctly");
    } else {
      require(callbacks == 0, "failed SPI operation invoked event callback");
      require(response == S_NAK && wire_reply_len == 0,
              "failed SPI operation did not send only NAK");
    }
  }
  puts("PASS: successful SPI callback; failed CS/write/read/deassert suppress callback and NAK");
  return 0;
}
