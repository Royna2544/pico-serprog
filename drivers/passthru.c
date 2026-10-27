#include "core/driver.h"

#include "core/devicetree.h"
#include <errno.h>

#include "drivers/passthru.h"
#include "drivers/uartdrv.h"
#include "drivers/usbcdcdrv.h"
#include "pico/stdlib.h"
#include <hardware/uart.h>

struct passthru_driver_pdata {
  struct device *uart_dev;
  struct device *usb_cdc_dev;
};

static int passthru_probe(struct device *dev) {
  struct device *uart =
      find_device_by_prop(dev, "uart", dev->fdt_node_offset, 0);
  if (!uart) {
    dev_err(dev, "Failed to find uart device");
    return -ENODEV;
  }
  struct device *usb_cdc =
      find_device_by_prop(dev, "usb-cdc", dev->fdt_node_offset, 0);
  if (!usb_cdc) {
    dev_err(dev, "Failed to find usb_cdc device");
    return -ENODEV;
  }

  struct passthru_driver_pdata *pdata = dev->platform_data =
      calloc(1, sizeof(*pdata));
  if (!pdata) {
    dev_err(dev, "Failed to allocate memory for passthru driver pdata");
    return -ENOMEM;
  }
  pdata->uart_dev = uart;
  pdata->usb_cdc_dev = usb_cdc;

  dev_info(dev, "Using UART device %s and USB CDC device %s for passthru",
           uart->name, usb_cdc->name);
  return 0;
}

static void passthru_remove(struct device *dev) {
  struct passthru_driver_pdata *pdata = dev->platform_data;
  if (pdata) {
    pdata->uart_dev = NULL;
    pdata->usb_cdc_dev = NULL;
    free(pdata);
    dev->platform_data = NULL;
  }
}

static void
passthru_encoding_changed_callback(usb_cdc_line_coding_t const *coding,
                                   void *data) {
  struct passthru_driver_pdata *pdata = (struct passthru_driver_pdata *)data;
  // Here you can handle the encoding change if needed'
  struct uart_line_coding uart_coding = {};
  // CDC maps data
  switch (coding->stop_bits) {
  case 0:
    uart_coding.stop_bits = 1;
    break;
  case 2:
    uart_coding.stop_bits = 2;
    break;
  case 1:
    dev_warn(pdata->uart_dev, "CDC stop bits 1.5 not supported, using 1");
    break;
  default:
    dev_warn(pdata->uart_dev, "CDC stop bits %d not supported",
             coding->stop_bits);
    break;
  }
  switch (coding->parity) {
  case 0:
    uart_coding.parity = UART_PARITY_NONE;
    break;
  case 1:
    uart_coding.parity = UART_PARITY_ODD;
    break;
  case 2:
    uart_coding.parity = UART_PARITY_EVEN;
    break;
  default:
    dev_warn(pdata->uart_dev, "Unsupported CDC parity %u", coding->parity);
    return;
  }
  uart_coding.data_bits = coding->data_bits;
  uart_set_line_coding(pdata->uart_dev, &uart_coding);
  uart_set_baud(pdata->uart_dev, coding->bit_rate);
  dev_info(pdata->uart_dev,
           "UART line coding changed: baud=%u, stop_bits=%d, parity=%d, "
           "data_bits=%d",
           coding->bit_rate, coding->stop_bits, coding->parity,
           coding->data_bits);
}

#include "tusb.h"
static void passthru_loop(struct device *dev, bool *stop) {
  struct passthru_driver_pdata *pdata = dev->platform_data;
  if (!pdata || !pdata->uart_dev || !pdata->usb_cdc_dev) {
    dev_err(dev, "Passthru loop: Invalid platform data");
    return;
  }

  struct usb_cdc_driver *usb_cdc_drv =
      container_of(pdata->usb_cdc_dev->drv, struct usb_cdc_driver, drv);
  if (!usb_cdc_drv) {
    dev_err(dev, "Passthru loop: USB CDC driver not found");
    return;
  }
  usb_cdc_drv->register_io_callbacks(pdata->usb_cdc_dev, NULL, NULL,
                                     passthru_encoding_changed_callback, pdata);

  while (!*stop) {
    if (uart_readable(pdata->uart_dev)) {
      uint8_t buf;
      int read = uart_read_nbytes(pdata->uart_dev, &buf, sizeof(buf));
      if (read > 0) {
        usb_cdc_write_bytes(pdata->usb_cdc_dev, &buf, read);
        usb_cdc_write_flush(pdata->usb_cdc_dev);
      }
    }
    if (usb_cdc_read_available(pdata->usb_cdc_dev)) {
      uint8_t buf;
      int usb_read = usb_cdc_read_bytes(pdata->usb_cdc_dev, &buf, sizeof(buf));
      if (usb_read > 0) {
        uart_write_nbytes(pdata->uart_dev, &buf, usb_read);
      }
    }
  }
}

static const struct driver_fdt_match_id passthru_match[] = {
    {.compatible = "royna,passthru", .data = NULL}, {}};
static struct passthru_driver passthru_drv = {
    .drv =
        {
            .name = "passthru-drv",
            .probe = passthru_probe,
            .remove = passthru_remove,
            .match_table = passthru_match,
        },
    .loop = passthru_loop,
};

PLATFORM_DRIVER(passthru_drv);

void passthru_handle_loop(struct device *dev, bool *stop) {
  struct passthru_driver *drv =
      container_of(dev->drv, struct passthru_driver, drv);
  if (drv && drv->loop) {
    drv->loop(dev, stop);
  } else {
    dev_err(dev, "Passthru handle loop: No loop function defined");
  }
}