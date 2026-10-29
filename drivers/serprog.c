#include "serprog.h"
#include "core/devicetree.h"
#include "drivers/spidrv.h"
#include "drivers/usbcdcdrv.h"
#include <sys/errno.h>

#include "serprog_defs.h"

/* Bounded in RAM, but larger than the CDC packet size. Both SPI directions
 * accept exactly this many bytes; larger requests are drained and NAKed. */
#define SPI_MAX_LEN 4096u
#define SPI_CHUNK_SIZE 256u
#define SERIAL_BUFFER_SIZE 64u
static uint8_t spi_send_buffer[SPI_MAX_LEN];
static uint8_t spi_recv_buffer[SPI_MAX_LEN];

struct serprog_platform_data {
  const struct device *usb_cdc;
  const struct device *spi_chip;
  spi_transfer_callback_t spi_transfer_callback;
  serprog_command_callback_t serprog_command_callback;
};

static int serprogdrv_probe(struct device *dev) {
  dev_info(dev, "Probing serprog driver");

  const struct device *usb_cdc =
      find_device_by_prop(dev, "usb-cdc", dev->fdt_node_offset, 0);
  if (!usb_cdc) {
    dev_err(dev, "Failed to find USB CDC device for serprog driver");
    return -ENODEV;
  }

  const struct device *spi_dev =
      find_device_by_prop(dev, "spi-chip", dev->fdt_node_offset, 0);
  if (!spi_dev) {
    dev_err(dev, "Failed to find SPI chip for serprog driver");
    return -ENODEV;
  }
  dev_info(dev,
           "Found USB CDC device %s and SPI chip %s, attached to %s for "
           "serprog driver",
           usb_cdc->name, spi_dev->name,
           spi_dev->parent ? spi_dev->parent->name : "(null)");

  struct serprog_platform_data *pdata = calloc(1, sizeof(*pdata));
  if (!pdata) {
    dev_err(dev, "Failed to allocate serprog platform data");
    return -ENOMEM;
  }
  pdata->usb_cdc = usb_cdc;
  pdata->spi_chip = spi_dev;
  dev->platform_data = pdata;
  return 0;
}

static void serprogdrv_remove(struct device *dev) {
  dev_info(dev, "Removing serprog driver");
  free(dev->platform_data);
  dev->platform_data = NULL;
}

static void
serprogdrv_register_xfer_callback(const struct device *dev,
                                  spi_transfer_callback_t callback) {
  struct serprog_platform_data *pdata = dev->platform_data;
  if (!pdata) {
    dev_err(dev, "No platform data for serprog driver");
    return;
  }
  pdata->spi_transfer_callback = callback;
}

static void
serprogdrv_register_serprog_cmd_callback(const struct device *dev,
                                         serprog_command_callback_t callback) {
  struct serprog_platform_data *pdata = dev->platform_data;
  if (!pdata) {
    dev_err(dev, "No platform data for serprog driver");
    return;
  }
  pdata->serprog_command_callback = callback;
}

static uint32_t get_le(const uint8_t *bytes, unsigned count) {
  uint32_t value = 0;
  for (unsigned i = 0; i < count; i++)
    value |= (uint32_t)bytes[i] << (8 * i);
  return value;
}

static void put_le(const struct device *cdc, uint32_t value, unsigned count) {
  uint8_t bytes[4];
  for (unsigned i = 0; i < count; i++)
    bytes[i] = (uint8_t)(value >> (8 * i));
  usb_cdc_write_bytes(cdc, bytes, count);
}

static bool discard_bytes(const struct device *cdc, uint32_t count) {
  uint8_t buffer[SPI_CHUNK_SIZE];
  while (count) {
    uint32_t n = count < sizeof(buffer) ? count : sizeof(buffer);
    if (usb_cdc_read_bytes(cdc, buffer, n) != n)
      return false;
    count -= n;
  }
  return true;
}

/* Read the complete send payload before asserting CS, then keep one CS edge
 * around all SPI chunks. Collect the complete reply before sending ACK. */
static void handle_spi(const struct serprog_platform_data *pdata) {
  const struct device *cdc = pdata->usb_cdc;
  uint8_t lengths[6];
  if (usb_cdc_read_bytes(cdc, lengths, sizeof(lengths)) != sizeof(lengths))
    return;
  uint32_t slen = get_le(lengths, 3);
  uint32_t rlen = get_le(lengths + 3, 3);

  if (slen > SPI_MAX_LEN || rlen > SPI_MAX_LEN) {
    if (discard_bytes(cdc, slen))
      usb_cdc_write_byte(cdc, S_NAK);
    return;
  }

  uint8_t *send = (slen ? spi_send_buffer : NULL);
  uint8_t *reply = (rlen ? spi_recv_buffer : NULL);
  if (slen && usb_cdc_read_bytes(cdc, send, slen) != slen) {
    return;
  }

  bool ok = spi_chip_assert_cs(pdata->spi_chip) == 0;
  for (uint32_t pos = 0; pos < slen;) {
    uint32_t chunk = slen - pos < SPI_CHUNK_SIZE ? slen - pos : SPI_CHUNK_SIZE;
    if (ok &&
        spi_chip_write_nbytes(pdata->spi_chip, send + pos, chunk) != (int)chunk)
      ok = false;
    pos += chunk;
  }
  if (ok) {
    for (uint32_t pos = 0; pos < rlen;) {
      uint32_t chunk =
          rlen - pos < SPI_CHUNK_SIZE ? rlen - pos : SPI_CHUNK_SIZE;
      if (spi_chip_read_nbytes(pdata->spi_chip, reply + pos, chunk) !=
          (int)chunk) {
        ok = false;
        break;
      }
      pos += chunk;
    }
  }
  if (spi_chip_deassert_cs(pdata->spi_chip) != 0)
    ok = false;

  if (ok && pdata->spi_transfer_callback)
    pdata->spi_transfer_callback(pdata->spi_chip, send, slen, reply, rlen);

  usb_cdc_write_byte(cdc, ok ? S_ACK : S_NAK);
  if (ok && rlen)
    usb_cdc_write_bytes(cdc, reply, rlen);
}

static void serprogdrv_handle_command(const struct device *dev) {
  struct serprog_platform_data *pdata = dev->platform_data;
  if (!pdata) {
    dev_err(dev, "No platform data for serprog driver");
    return;
  }
  const struct device *cdc = pdata->usb_cdc;
  uint8_t cmd = 0;
  if (usb_cdc_read_byte(cdc, &cmd) != 1) {
    dev_err(dev, "Failed to read command byte from USB CDC");
    return;
  }

  switch (cmd) {
  case S_CMD_NOP:
    usb_cdc_write_byte(cdc, S_ACK);
    break;
  case S_CMD_Q_IFACE:
    usb_cdc_write_byte(cdc, S_ACK);
    put_le(cdc, 1, 2);
    break;
  case S_CMD_Q_RDNMAXLEN:
  case S_CMD_Q_WRNMAXLEN:
    usb_cdc_write_byte(cdc, S_ACK);
    put_le(cdc, SPI_MAX_LEN, 3);
    break;
  case S_CMD_Q_CMDMAP: {
    /* bit i advertises only a command that actually returns the spec reply. */
    static const uint8_t cmdmap[32] = {
        0x3f,
        0x01,
        0x1f, /* 0..5, 8, 16..20 */
    };
    usb_cdc_write_byte(cdc, S_ACK);
    usb_cdc_write_bytes(cdc, cmdmap, sizeof(cmdmap));
    break;
  }
  case S_CMD_Q_PGMNAME: {
    static const char progname[16] = "pico-serprog-w+";
    usb_cdc_write_byte(cdc, S_ACK);
    usb_cdc_write_bytes(cdc, progname, sizeof(progname));
    break;
  }
  case S_CMD_Q_SERBUF:
    usb_cdc_write_byte(cdc, S_ACK);
    put_le(cdc, SERIAL_BUFFER_SIZE, 2);
    break;
  case S_CMD_Q_BUSTYPE:
    usb_cdc_write_byte(cdc, S_ACK);
    usb_cdc_write_byte(cdc, 1 << 3); // SPI only
    break;
  case S_CMD_SYNCNOP:
    usb_cdc_write_byte(cdc, S_NAK);
    usb_cdc_write_byte(cdc, S_ACK);
    break;
  case S_CMD_S_BUSTYPE: {
    uint8_t bus;
    if (usb_cdc_read_byte(cdc, &bus) != 1)
      break;
    usb_cdc_write_byte(cdc, bus & (1 << 3) ? S_ACK : S_NAK);
    break;
  }
  case S_CMD_O_SPIOP:
    handle_spi(pdata);
    break;
  case S_CMD_S_SPI_FREQ: {
    uint8_t bytes[4];
    if (usb_cdc_read_bytes(cdc, bytes, sizeof(bytes)) != sizeof(bytes))
      break;
    uint32_t want = get_le(bytes, 4);
    if (!want || spi_chip_set_baud(pdata->spi_chip, want) != 0) {
      usb_cdc_write_byte(cdc, S_NAK);
      break;
    }
    /* set_baud returns status, not the actual frequency. Read hardware back. */
    uint32_t actual = spi_chip_get_baud(pdata->spi_chip);
    if (!actual) {
      usb_cdc_write_byte(cdc, S_NAK);
      break;
    }
    usb_cdc_write_byte(cdc, S_ACK);
    put_le(cdc, actual, 4);
    break;
  }
  case S_CMD_R_BYTE:
    if (!discard_bytes(cdc, 3))
      break;
    usb_cdc_write_byte(cdc, S_NAK);
    break;
  case S_CMD_R_NBYTES:
    if (!discard_bytes(cdc, 6))
      break;
    usb_cdc_write_byte(cdc, S_NAK);
    break;
  case S_CMD_O_WRITEB:
    if (!discard_bytes(cdc, 4))
      break;
    usb_cdc_write_byte(cdc, S_NAK);
    break;
  case S_CMD_O_WRITEN: {
    uint8_t bytes[3];
    if (usb_cdc_read_bytes(cdc, bytes, 3) != 3)
      break;
    uint32_t n = get_le(bytes, 3);
    if (!discard_bytes(cdc, n + 3))
      break;
    usb_cdc_write_byte(cdc, S_NAK);
    break;
  }
  case S_CMD_O_DELAY:
    if (!discard_bytes(cdc, 4))
      break;
    usb_cdc_write_byte(cdc, S_NAK);
    break;
  case S_CMD_S_PIN_STATE:
  case 0x16: /* S_SPI_CS: no alternate chip selects */
  case 0x17: /* S_SPI_MODE: half duplex only */
  case 0x18: /* S_CS_MODE: automatic CS only */
    if (!discard_bytes(cdc, 1))
      break;
    usb_cdc_write_byte(cdc, S_NAK);
    break;
  case S_CMD_O_INIT:
  case S_CMD_O_EXEC:
  case S_CMD_Q_OPBUF:
  case S_CMD_Q_CHIPSIZE:
    usb_cdc_write_byte(cdc, S_NAK);
    break;
  default:
    usb_cdc_write_byte(cdc, S_NAK);
    break;
  }
  usb_cdc_write_flush(cdc);

  if (pdata->serprog_command_callback)
    pdata->serprog_command_callback(dev, cmd);
}

static const struct driver_fdt_match_id serprog_driver_ids[] = {
    {.compatible = "royna,serprog"},
    {},
};

static struct serprog_driver serprog_driver = {
    .drv =
        {
            .name = "rp2040-serprog-drv",
            .probe = serprogdrv_probe,
            .remove = serprogdrv_remove,
            .match_table = serprog_driver_ids,
        },
    .handle_command = serprogdrv_handle_command,
    .register_xfer_callback = serprogdrv_register_xfer_callback,
    .register_serprog_cmd_callback = serprogdrv_register_serprog_cmd_callback,
};

PLATFORM_DRIVER(serprog_driver);

void serprogdrv_process_commands(const struct device *dev) {
  struct serprog_platform_data *pdata = dev->platform_data;
  if (!pdata) {
    dev_err(dev, "No platform data for serprog driver");
    return;
  }

  while (1) {
    serprogdrv_handle_command(dev);
  }
}

void serprogdrv_set_spi_transfer_callback(const struct device *dev,
                                          spi_transfer_callback_t callback) {
  struct serprog_platform_data *pdata = dev->platform_data;
  if (!pdata) {
    dev_err(dev, "No platform data for serprog driver");
    return;
  }
  pdata->spi_transfer_callback = callback;
}

void serprogdrv_set_command_callback(const struct device *dev,
                                     serprog_command_callback_t callback) {
  struct serprog_platform_data *pdata = dev->platform_data;
  if (!pdata) {
    dev_err(dev, "No platform data for serprog driver");
    return;
  }
  pdata->serprog_command_callback = callback;
}