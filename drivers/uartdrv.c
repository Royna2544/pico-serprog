#include "uartdrv.h"
#include "../core/driver.h"
#include "../core/init.h"
#include "core/devicetree.h"
#include <hardware/gpio.h>
#include <hardware/uart.h>
#include <pico/stdio.h>
#include <pico/stdio_uart.h>
#include <sys/errno.h>

#include <libfdt.h>

struct uart_platform_data {
  void *hw;
  unsigned int default_baud, current_baud;
  bool acquired;
};

static int uartdrv_probe(struct device *dev) {
  struct uart_platform_data *pdata = calloc(1, sizeof(*pdata));
  if (!pdata) {
    dev_err(dev, "%s: Failed to allocate platform data", __func__);
    return -ENOMEM;
  }
  dev->platform_data = pdata;

  if (get_prop_uint32_default(dev, "default-baud", &pdata->default_baud,
                              115200))
    return -EINVAL;

  struct reg reg_info;
  if (get_reg(dev, NULL, &reg_info)) {
    return -EINVAL;
  }
  int base = reg_info.base;

  uart_inst_t *uart;

  switch (base) {
  case UART0_BASE:
    uart = uart0;
    break;
  case UART1_BASE:
    uart = uart1;
    break;
  default:
    dev_err(dev, "Unknown UART controller: 0x%08x", base);
    return -ENODEV;
  }
  pdata->hw = uart;
  pdata->acquired = false;
  dev_dbg(dev, "%s: def. baud: %u", __func__, pdata->default_baud);

  if (!uart_is_enabled(pdata->hw)) {
    pdata->current_baud = uart_init(pdata->hw, pdata->default_baud);
    pdata->acquired = true;
    dev_dbg(dev, "%s: initialized UART instance", __func__);
  } else {
    dev_dbg(dev, "%s: Taking over existing UART instance", __func__);
    pdata->acquired = true;
    pdata->current_baud = uart_set_baudrate(pdata->hw, pdata->default_baud);
  }

  return 0;
}

static void uartdrv_remove(struct device *dev) {
  const struct uart_platform_data *pdata = dev->platform_data;

  uart_deinit(pdata->hw);
  dev_dbg(dev, "%s: removed", __func__);
  free(dev->platform_data);
  dev->platform_data = NULL;
}

static bool uartdrv_readable(const struct device *dev) {
  const struct uart_platform_data *pdata = dev->platform_data;
  return pdata->acquired && uart_is_readable(pdata->hw);
}
static bool uartdrv_writable(const struct device *dev) {
  const struct uart_platform_data *pdata = dev->platform_data;
  return pdata->acquired && uart_is_writable(pdata->hw);
}

static int uartdrv_read_nbytes(const struct device *dev, uint8_t *dst,
                               size_t len) {
  const struct uart_platform_data *pdata = dev->platform_data;
  if (pdata->acquired) {
#ifdef MOCK_HARDWARE
    return uart_read_blocking(pdata->hw, dst, len);
#else
    uart_read_blocking(pdata->hw, dst, len);
    return len;
#endif
  } else {
    dev_err(dev, "%s: UART not acquired", __func__);
    return -ENODEV;
  }
}

int uartdrv_write_nbytes(const struct device *dev, const uint8_t *src,
                         size_t len) {
  const struct uart_platform_data *pdata = dev->platform_data;
  if (pdata->acquired) {
#ifdef MOCK_HARDWARE
    return uart_write_blocking(pdata->hw, src, len);
#else
    uart_write_blocking(pdata->hw, src, len);
    return len;
#endif
  } else {
    dev_err(dev, "%s: UART not acquired", __func__);
    return -ENODEV;
  }
}

int uartdrv_read_nbytes_nonblocking(const struct device *dev, uint8_t *dst,
                                    size_t len) {
  const struct uart_platform_data *pdata = dev->platform_data;
  if (pdata->acquired) {
    size_t bytes_read = 0;
    while (bytes_read < len && uart_is_readable(pdata->hw)) {
#ifdef MOCK_HARDWARE
      int byte = uart_getc(pdata->hw);
      if (byte < 0)
        return byte;
      dst[bytes_read++] = (uint8_t)byte;
#else
      dst[bytes_read++] = uart_getc(pdata->hw);
#endif
    }
    return bytes_read;
  } else {
    dev_err(dev, "%s: UART not acquired", __func__);
    return -ENODEV;
  }
}

int uartdrv_write_nbytes_nonblocking(const struct device *dev,
                                     const uint8_t *src, size_t len) {
  const struct uart_platform_data *pdata = dev->platform_data;
  if (pdata->acquired) {
    size_t bytes_written = 0;
    while (bytes_written < len && uart_is_writable(pdata->hw)) {
#ifdef MOCK_HARDWARE
      /* Mock uart_putc sign-extends 0xff to -1; use the byte-count API. */
      int written = uart_write_blocking(pdata->hw, &src[bytes_written], 1);
      if (written < 0)
        return written;
      if (written == 0)
        break;
      bytes_written++;
#else
      uart_putc(pdata->hw, src[bytes_written++]);
#endif
    }
    return bytes_written;
  } else {
    dev_err(dev, "%s: UART not acquired", __func__);
    return -ENODEV;
  }
}

void uartdrv_set_baud(const struct device *dev, uint baud) {
  struct uart_platform_data *pdata = dev->platform_data;
  if (pdata->acquired) {
    if (pdata->current_baud == baud) {
      dev_dbg(dev, "%s: baud already set to %u", __func__, baud);
    }
    pdata->current_baud = uart_set_baudrate(pdata->hw, baud);
    dev_dbg(dev, "%s: set baud to %u: accepted %u", __func__, baud,
            pdata->current_baud);
  } else {
    dev_err(dev, "%s: UART not acquired", __func__);
  }
}

void uartdrv_get_baud(const struct device *dev, unsigned int *baud) {
  struct uart_platform_data *pdata = dev->platform_data;
  if (pdata->acquired) {
    *baud = pdata->current_baud;
  } else {
    dev_err(dev, "%s: UART not acquired", __func__);
  }
}

void uartdrv_set_line_coding(const struct device *dev,
                             const uart_line_coding_t *coding) {
  struct uart_platform_data *pdata = dev->platform_data;
  if (pdata->acquired) {
    uart_set_format(pdata->hw, coding->data_bits, coding->stop_bits,
                    coding->parity);
  } else {
    dev_err(dev, "%s: UART not acquired", __func__);
  }
}

void uartdrv_release(const struct device *dev) {
  struct uart_platform_data *pdata = dev->platform_data;
  if (pdata->acquired) {
    if (remove_pinctrl_default(dev)) {
      dev_err(dev, "%s: Failed to remove pinctrl default", __func__);
      return;
    }
    uart_deinit(pdata->hw);
    pdata->acquired = false;
    dev_dbg(dev, "%s: UART released", __func__);
  } else {
    dev_err(dev, "%s: UART not acquired", __func__);
  }
}

void uartdrv_acquire(const struct device *dev) {
  struct uart_platform_data *pdata = dev->platform_data;
  if (!pdata->acquired) {
    pdata->current_baud = uart_init(pdata->hw, pdata->default_baud);
    if (apply_pinctrl_default(dev)) {
      dev_err(dev, "%s: Failed to apply pinctrl default", __func__);
      return;
    }
    pdata->acquired = true;
    dev_dbg(dev, "%s: UART acquired", __func__);
  } else {
    dev_err(dev, "%s: UART already acquired", __func__);
  }
}

const static struct driver_fdt_match_id pico_uart_match_table[] = {
    {.compatible = "raspberrypi,rp2040-uart", .data = 0}, {}};

struct uart_driver pico_uart_driver = {
    .drv = (struct driver){.name = "pico-uart-drv",
                           .probe = uartdrv_probe,
                           .remove = uartdrv_remove,
                           .match_table = pico_uart_match_table},
    .read_nbytes = uartdrv_read_nbytes,
    .write_nbytes = uartdrv_write_nbytes,
    .read_nbytes_nonblocking = uartdrv_read_nbytes_nonblocking,
    .write_nbytes_nonblocking = uartdrv_write_nbytes_nonblocking,
    .readable = uartdrv_readable,
    .writable = uartdrv_writable,
    .set_baud = uartdrv_set_baud,
    .get_baud = uartdrv_get_baud,
    .set_line_coding = uartdrv_set_line_coding,
    .release = uartdrv_release,
    .acquire = uartdrv_acquire,
};

PLATFORM_DRIVER(pico_uart_driver);

int uart_write_nbytes(const struct device *device, const uint8_t *src,
                      size_t len) {
  struct uart_driver *drv = container_of(device->drv, struct uart_driver, drv);
  if (!drv || !drv->write_nbytes) {
    dev_err(device, "UART driver does not support write_nbytes");
    return -ENOSYS;
  }
  return drv->write_nbytes(device, src, len);
}

int uart_read_nbytes(const struct device *device, uint8_t *dst, size_t len) {
  struct uart_driver *drv = container_of(device->drv, struct uart_driver, drv);
  if (!drv || !drv->read_nbytes) {
    dev_err(device, "UART driver does not support read_nbytes");
    return -ENOSYS;
  }
  return drv->read_nbytes(device, dst, len);
}

int uart_write_nbytes_nonblocking(const struct device *device,
                                  const uint8_t *src, size_t len) {
  struct uart_driver *drv = container_of(device->drv, struct uart_driver, drv);
  if (!drv || !drv->write_nbytes_nonblocking) {
    dev_err(device, "UART driver does not support write_nbytes_nonblocking");
    return -ENOSYS;
  }
  return drv->write_nbytes_nonblocking(device, src, len);
}

int uart_read_nbytes_nonblocking(const struct device *device, uint8_t *dst,
                                 size_t len) {
  struct uart_driver *drv = container_of(device->drv, struct uart_driver, drv);
  if (!drv || !drv->read_nbytes_nonblocking) {
    dev_err(device, "UART driver does not support read_nbytes_nonblocking");
    return -ENOSYS;
  }
  return drv->read_nbytes_nonblocking(device, dst, len);
}

bool uart_readable(const struct device *device) {
  struct uart_driver *drv = container_of(device->drv, struct uart_driver, drv);
  if (!drv || !drv->readable) {
    dev_err(device, "UART driver does not support readable");
    return false;
  }
  return drv->readable(device);
}

bool uart_writable(const struct device *device) {
  struct uart_driver *drv = container_of(device->drv, struct uart_driver, drv);
  if (!drv || !drv->writable) {
    dev_err(device, "UART driver does not support writable");
    return false;
  }
  return drv->writable(device);
}

void uart_set_baud(const struct device *device, unsigned int baud) {
  struct uart_driver *drv = container_of(device->drv, struct uart_driver, drv);
  if (!drv || !drv->set_baud) {
    dev_err(device, "UART driver does not support set_baud");
    return;
  }
  drv->set_baud(device, baud);
}

void uart_get_baud(const struct device *device, unsigned int *baud) {
  struct uart_driver *drv = container_of(device->drv, struct uart_driver, drv);
  if (!drv || !drv->get_baud) {
    dev_err(device, "UART driver does not support get_baud");
    return;
  }
  drv->get_baud(device, baud);
}

void uart_set_line_coding(const struct device *device,
                          const uart_line_coding_t *coding) {
  struct uart_driver *drv = container_of(device->drv, struct uart_driver, drv);
  if (!drv || !drv->set_line_coding) {
    dev_err(device, "UART driver does not support set_line_coding");
    return;
  }
  drv->set_line_coding(device, coding);
}

void uart_release(const struct device *device) {
  struct uart_driver *drv = container_of(device->drv, struct uart_driver, drv);
  if (!drv || !drv->release) {
    dev_err(device, "UART driver does not support release");
    return;
  }
  drv->release(device);
}

void uart_acquire(const struct device *device) {
  struct uart_driver *drv = container_of(device->drv, struct uart_driver, drv);
  if (!drv || !drv->acquire) {
    dev_err(device, "UART driver does not support acquire");
    return;
  }
  drv->acquire(device);
}

int uart_read_nbytes_until(const struct device *device, uint8_t *dst,
                           size_t len, uint8_t terminator) {
  struct uart_driver *drv = container_of(device->drv, struct uart_driver, drv);
  if (!drv || !drv->read_nbytes_nonblocking) {
    dev_err(device, "UART driver does not support read_nbytes_nonblocking");
    return -ENOSYS;
  }

  size_t bytes_read = 0;
  while (bytes_read < len) {
    int ret = drv->read_nbytes_nonblocking(device, &dst[bytes_read], 1);
    if (ret < 0) {
      return ret; // Error occurred
    } else if (ret == 0) {
      continue; // No data available, try again
    }

    if (dst[bytes_read] == terminator) {
      break; // Terminator found
    }
    bytes_read++;
  }
  return bytes_read;
}